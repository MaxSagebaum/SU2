/*!
 * \file CNSSolver.cpp
 * \brief Main subrotuines for solving Finite-Volume Navier-Stokes flow problems.
 * \author F. Palacios, T. Economon
 * \version 7.0.8 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2020, SU2 Contributors (cf. AUTHORS.md)
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../include/solvers/CNSSolver.hpp"
#include "../../include/variables/CNSVariable.hpp"
#include "../../../Common/include/toolboxes/printing_toolbox.hpp"
#include "../../../Common/include/toolboxes/geometry_toolbox.hpp"
#include "../../include/solvers/CFVMFlowSolverBase.inl"

/*--- Explicit instantiation of the parent class of CEulerSolver,
 *    to spread the compilation over two cpp files. ---*/
template class CFVMFlowSolverBase<CEulerVariable, COMPRESSIBLE>;


CNSSolver::CNSSolver(CGeometry *geometry, CConfig *config, unsigned short iMesh) :
           CEulerSolver(geometry, config, iMesh, true) {

  /*--- This constructor only allocates/inits what is extra to CEulerSolver. ---*/

  /*--- Allocates a 2D array with variable "outer" sizes and init to 0. ---*/

  auto Alloc2D = [](unsigned long M, const unsigned long* N, su2double**& X) {
    X = new su2double* [M];
    for(unsigned long i = 0; i < M; ++i)
      X[i] = new su2double [N[i]] ();
  };

  /*--- Buffet sensor in all the markers and coefficients ---*/

  if (config->GetBuffet_Monitoring() || config->GetKind_ObjFunc() == BUFFET_SENSOR){

    Alloc2D(nMarker, nVertex, Buffet_Sensor);
    Buffet_Metric = new su2double[nMarker];
    Surface_Buffet_Metric = new su2double[config->GetnMarker_Monitoring()];

  }

  /*--- Read farfield conditions from config ---*/

  Viscosity_Inf   = config->GetViscosity_FreeStreamND();
  Prandtl_Lam     = config->GetPrandtl_Lam();
  Prandtl_Turb    = config->GetPrandtl_Turb();
  Tke_Inf         = config->GetTke_FreeStreamND();

  /*--- Initialize the seed values for forward mode differentiation. ---*/

  switch(config->GetDirectDiff()) {
    case D_VISCOSITY:
      SU2_TYPE::SetDerivative(Viscosity_Inf, 1.0);
      break;
    default:
      /*--- Already done upstream. ---*/
      break;
  }

}

CNSSolver::~CNSSolver(void) {

  unsigned short iMarker;

  delete [] Buffet_Metric;
  delete [] Surface_Buffet_Metric;

  if (Buffet_Sensor != nullptr) {
    for (iMarker = 0; iMarker < nMarker; iMarker++){
      delete [] Buffet_Sensor[iMarker];
    }
    delete [] Buffet_Sensor;
  }

}

void CNSSolver::Preprocessing(CGeometry *geometry, CSolver **solver_container, CConfig *config, unsigned short iMesh,
                              unsigned short iRKStep, unsigned short RunTime_EqSystem, bool Output) {

  unsigned long InnerIter   = config->GetInnerIter();
  bool cont_adjoint         = config->GetContinuous_Adjoint();
  bool limiter_flow         = (config->GetKind_SlopeLimit_Flow() != NO_LIMITER) && (InnerIter <= config->GetLimiterIter());
  bool limiter_turb         = (config->GetKind_SlopeLimit_Turb() != NO_LIMITER) && (InnerIter <= config->GetLimiterIter());
  bool limiter_adjflow      = (cont_adjoint && (config->GetKind_SlopeLimit_AdjFlow() != NO_LIMITER) && (InnerIter <= config->GetLimiterIter()));
  bool van_albada           = config->GetKind_SlopeLimit_Flow() == VAN_ALBADA_EDGE;
  bool wall_functions       = config->GetWall_Functions();

  /*--- Common preprocessing steps (implemented by CEulerSolver) ---*/

  CommonPreprocessing(geometry, solver_container, config, iMesh, iRKStep, RunTime_EqSystem, Output);

  /*--- Compute gradient for MUSCL reconstruction. ---*/

  if (config->GetReconstructionGradientRequired() && (iMesh == MESH_0)) {
    switch (config->GetKind_Gradient_Method_Recon()) {
      case GREEN_GAUSS:
        SetPrimitive_Gradient_GG(geometry, config, true); break;
      case LEAST_SQUARES:
      case WEIGHTED_LEAST_SQUARES:
        SetPrimitive_Gradient_LS(geometry, config, true); break;
      default: break;
    }
  }

  /*--- Compute gradient of the primitive variables ---*/

  if (config->GetKind_Gradient_Method() == GREEN_GAUSS) {
    SetPrimitive_Gradient_GG(geometry, config);
  }
  else if (config->GetKind_Gradient_Method() == WEIGHTED_LEAST_SQUARES) {
    SetPrimitive_Gradient_LS(geometry, config);
  }

  /*--- Compute the limiter in case we need it in the turbulence model or to limit the
   *    viscous terms (check this logic with JST and 2nd order turbulence model) ---*/

  if ((iMesh == MESH_0) && (limiter_flow || limiter_turb || limiter_adjflow) && !Output && !van_albada) {
    SetPrimitive_Limiter(geometry, config);
  }

  /*--- Evaluate the vorticity and strain rate magnitude ---*/

  SU2_OMP_MASTER
  {
    StrainMag_Max = 0.0;
    Omega_Max = 0.0;
  }
  SU2_OMP_BARRIER

  nodes->SetVorticity_StrainMag();

  su2double strainMax = 0.0, omegaMax = 0.0;

  SU2_OMP(for schedule(static,omp_chunk_size) nowait)
  for (unsigned long iPoint = 0; iPoint < nPoint; iPoint++) {

    su2double StrainMag = nodes->GetStrainMag(iPoint);
    const su2double* Vorticity = nodes->GetVorticity(iPoint);
    su2double Omega = sqrt(Vorticity[0]*Vorticity[0]+ Vorticity[1]*Vorticity[1]+ Vorticity[2]*Vorticity[2]);

    strainMax = max(strainMax, StrainMag);
    omegaMax = max(omegaMax, Omega);

  }
  SU2_OMP_CRITICAL
  {
    StrainMag_Max = max(StrainMag_Max, strainMax);
    Omega_Max = max(Omega_Max, omegaMax);
  }

  if ((iMesh == MESH_0) && (config->GetComm_Level() == COMM_FULL)) {
    SU2_OMP_BARRIER
    SU2_OMP_MASTER
    {
      su2double MyOmega_Max = Omega_Max;
      su2double MyStrainMag_Max = StrainMag_Max;

      SU2_MPI::Allreduce(&MyStrainMag_Max, &StrainMag_Max, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
      SU2_MPI::Allreduce(&MyOmega_Max, &Omega_Max, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    }
    SU2_OMP_BARRIER
  }

  /*--- Compute the TauWall from the wall functions ---*/

  if (wall_functions) {
    SetTauWall_WF(geometry, solver_container, config);
  }

}

unsigned long CNSSolver::SetPrimitive_Variables(CSolver **solver_container, CConfig *config, bool Output) {

  /*--- Number of non-physical points, local to the thread, needs
   *    further reduction if function is called in parallel ---*/
  unsigned long nonPhysicalPoints = 0;

  const unsigned short turb_model = config->GetKind_Turb_Model();
  const bool tkeNeeded = (turb_model == SST) || (turb_model == SST_SUST);

  SU2_OMP_FOR_STAT(omp_chunk_size)
  for (unsigned long iPoint = 0; iPoint < nPoint; iPoint ++) {

    /*--- Retrieve the value of the kinetic energy (if needed). ---*/

    su2double eddy_visc = 0.0, turb_ke = 0.0;

    if (turb_model != NONE && solver_container[TURB_SOL] != nullptr) {
      eddy_visc = solver_container[TURB_SOL]->GetNodes()->GetmuT(iPoint);
      if (tkeNeeded) turb_ke = solver_container[TURB_SOL]->GetNodes()->GetSolution(iPoint,0);

      if (config->GetKind_HybridRANSLES() != NO_HYBRIDRANSLES) {
        su2double DES_LengthScale = solver_container[TURB_SOL]->GetNodes()->GetDES_LengthScale(iPoint);
        nodes->SetDES_LengthScale(iPoint, DES_LengthScale);
      }
    }

    /*--- Compressible flow, primitive variables nDim+5, (T, vx, vy, vz, P, rho, h, c, lamMu, eddyMu, ThCond, Cp) ---*/

    bool physical = static_cast<CNSVariable*>(nodes)->SetPrimVar(iPoint, eddy_visc, turb_ke, GetFluidModel());
    nodes->SetSecondaryVar(iPoint, GetFluidModel());

    /*--- Check for non-realizable states for reporting. ---*/

    nonPhysicalPoints += !physical;

  }

  return nonPhysicalPoints;
}

void CNSSolver::Viscous_Residual(unsigned long iEdge, CGeometry *geometry, CSolver **solver_container,
                                 CNumerics *numerics, CConfig *config) {

  const bool implicit  = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  const bool tkeNeeded = (config->GetKind_Turb_Model() == SST) ||
                         (config->GetKind_Turb_Model() == SST_SUST);

  CVariable* turbNodes = nullptr;
  if (tkeNeeded) turbNodes = solver_container[TURB_SOL]->GetNodes();

  /*--- Points, coordinates and normal vector in edge ---*/

  auto iPoint = geometry->edges->GetNode(iEdge,0);
  auto jPoint = geometry->edges->GetNode(iEdge,1);

  numerics->SetCoord(geometry->nodes->GetCoord(iPoint),
                     geometry->nodes->GetCoord(jPoint));

  numerics->SetNormal(geometry->edges->GetNormal(iEdge));

  /*--- Primitive and secondary variables. ---*/

  numerics->SetPrimitive(nodes->GetPrimitive(iPoint),
                         nodes->GetPrimitive(jPoint));

  numerics->SetSecondary(nodes->GetSecondary(iPoint),
                         nodes->GetSecondary(jPoint));

  /*--- Gradients. ---*/

  numerics->SetPrimVarGradient(nodes->GetGradient_Primitive(iPoint),
                               nodes->GetGradient_Primitive(jPoint));

  /*--- Turbulent kinetic energy. ---*/

  if (tkeNeeded)
    numerics->SetTurbKineticEnergy(turbNodes->GetSolution(iPoint,0),
                                   turbNodes->GetSolution(jPoint,0));

  /*--- Wall shear stress values (wall functions) ---*/

  numerics->SetTauWall(nodes->GetTauWall(iPoint),
                       nodes->GetTauWall(iPoint));

  /*--- Compute and update residual ---*/

  auto residual = numerics->ComputeResidual(config);

  if (ReducerStrategy) {
    EdgeFluxes.SubtractBlock(iEdge, residual);
    if (implicit)
      Jacobian.UpdateBlocksSub(iEdge, residual.jacobian_i, residual.jacobian_j);
  }
  else {
    LinSysRes.SubtractBlock(iPoint, residual);
    LinSysRes.AddBlock(jPoint, residual);

    if (implicit)
      Jacobian.UpdateBlocksSub(iEdge, iPoint, jPoint, residual.jacobian_i, residual.jacobian_j);
  }

}

void CNSSolver::Buffet_Monitoring(CGeometry *geometry, CConfig *config) {

  unsigned long iVertex;
  unsigned short Boundary, Monitoring, iMarker, iMarker_Monitoring, iDim;
  su2double *Vel_FS = config->GetVelocity_FreeStream();
  su2double VelMag_FS = 0.0, SkinFrictionMag = 0.0, SkinFrictionDot = 0.0, *Normal, Area, Sref = config->GetRefArea();
  su2double k   = config->GetBuffet_k(),
             lam = config->GetBuffet_lambda();
  string Marker_Tag, Monitoring_Tag;

  for (iDim = 0; iDim < nDim; iDim++){
    VelMag_FS += Vel_FS[iDim]*Vel_FS[iDim];
  }
  VelMag_FS = sqrt(VelMag_FS);

  /*-- Variables initialization ---*/

  Total_Buffet_Metric = 0.0;

  for (iMarker_Monitoring = 0; iMarker_Monitoring < config->GetnMarker_Monitoring(); iMarker_Monitoring++) {
    Surface_Buffet_Metric[iMarker_Monitoring] = 0.0;
  }

  /*--- Loop over the Euler and Navier-Stokes markers ---*/

  for (iMarker = 0; iMarker < nMarker; iMarker++) {

    Buffet_Metric[iMarker] = 0.0;

    Boundary   = config->GetMarker_All_KindBC(iMarker);
    Monitoring = config->GetMarker_All_Monitoring(iMarker);

    if ((Boundary == HEAT_FLUX) || (Boundary == ISOTHERMAL) || (Boundary == HEAT_FLUX) || (Boundary == CHT_WALL_INTERFACE)) {

      /*--- Loop over the vertices to compute the buffet sensor ---*/

      for (iVertex = 0; iVertex < geometry->nVertex[iMarker]; iVertex++) {

        /*--- Perform dot product of skin friction with freestream velocity ---*/

        SkinFrictionMag = 0.0;
        SkinFrictionDot = 0.0;
        for(iDim = 0; iDim < nDim; iDim++){
          SkinFrictionMag += CSkinFriction[iMarker][iDim][iVertex]*CSkinFriction[iMarker][iDim][iVertex];
          SkinFrictionDot += CSkinFriction[iMarker][iDim][iVertex]*Vel_FS[iDim];
        }
        SkinFrictionMag = sqrt(SkinFrictionMag);

        /*--- Normalize the dot product ---*/

        SkinFrictionDot /= SkinFrictionMag*VelMag_FS;

        /*--- Compute Heaviside function ---*/

        Buffet_Sensor[iMarker][iVertex] = 1./(1. + exp(2.*k*(SkinFrictionDot + lam)));

        /*--- Integrate buffet sensor ---*/

        if(Monitoring == YES){

          Normal = geometry->vertex[iMarker][iVertex]->GetNormal();
          Area = GeometryToolbox::Norm(nDim, Normal);

          Buffet_Metric[iMarker] += Buffet_Sensor[iMarker][iVertex]*Area/Sref;

        }

      }

      if(Monitoring == YES){

        Total_Buffet_Metric += Buffet_Metric[iMarker];

        /*--- Per surface buffet metric ---*/

        for (iMarker_Monitoring = 0; iMarker_Monitoring < config->GetnMarker_Monitoring(); iMarker_Monitoring++) {
          Monitoring_Tag = config->GetMarker_Monitoring_TagBound(iMarker_Monitoring);
          Marker_Tag = config->GetMarker_All_TagBound(iMarker);
          if (Marker_Tag == Monitoring_Tag) Surface_Buffet_Metric[iMarker_Monitoring] = Buffet_Metric[iMarker];
        }

      }

    }

  }

#ifdef HAVE_MPI

  /*--- Add buffet metric information using all the nodes ---*/

  su2double MyTotal_Buffet_Metric = Total_Buffet_Metric;
  SU2_MPI::Allreduce(&MyTotal_Buffet_Metric, &Total_Buffet_Metric, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  /*--- Add the buffet metric on the surfaces using all the nodes ---*/

  su2double *MySurface_Buffet_Metric = new su2double[config->GetnMarker_Monitoring()];

  for (iMarker_Monitoring = 0; iMarker_Monitoring < config->GetnMarker_Monitoring(); iMarker_Monitoring++) {
    MySurface_Buffet_Metric[iMarker_Monitoring] = Surface_Buffet_Metric[iMarker_Monitoring];
  }

  SU2_MPI::Allreduce(MySurface_Buffet_Metric, Surface_Buffet_Metric, config->GetnMarker_Monitoring(), MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  delete [] MySurface_Buffet_Metric;

#endif

}

void CNSSolver::Evaluate_ObjFunc(CConfig *config) {

  unsigned short iMarker_Monitoring, Kind_ObjFunc;
  su2double Weight_ObjFunc;

  /*--- Evaluate objective functions common to Euler and NS solvers ---*/

  CEulerSolver::Evaluate_ObjFunc(config);

  /*--- Evaluate objective functions specific to NS solver ---*/

  for (iMarker_Monitoring = 0; iMarker_Monitoring < config->GetnMarker_Monitoring(); iMarker_Monitoring++) {

    Weight_ObjFunc = config->GetWeight_ObjFunc(iMarker_Monitoring);
    Kind_ObjFunc = config->GetKind_ObjFunc(iMarker_Monitoring);

    switch(Kind_ObjFunc) {
      case BUFFET_SENSOR:
          Total_ComboObj +=Weight_ObjFunc*Surface_Buffet_Metric[iMarker_Monitoring];
          break;
      default:
          break;
    }
  }

}

void CNSSolver::SetRoe_Dissipation(CGeometry *geometry, CConfig *config){

  const unsigned short kind_roe_dissipation = config->GetKind_RoeLowDiss();

  SU2_OMP_FOR_STAT(omp_chunk_size)
  for (unsigned long iPoint = 0; iPoint < nPoint; iPoint++) {

    if (kind_roe_dissipation == FD || kind_roe_dissipation == FD_DUCROS){

      su2double wall_distance = geometry->nodes->GetWall_Distance(iPoint);

      nodes->SetRoe_Dissipation_FD(iPoint, wall_distance);

    } else if (kind_roe_dissipation == NTS || kind_roe_dissipation == NTS_DUCROS) {

      const su2double delta = geometry->nodes->GetMaxLength(iPoint);
      assert(delta > 0 && "Delta must be initialized and non-negative");
      nodes->SetRoe_Dissipation_NTS(iPoint, delta, config->GetConst_DES());
    }
  }

}

void CNSSolver::AddDynamicGridResidualContribution(unsigned long iPoint, unsigned long Point_Normal,
                                                   CGeometry* geometry,  const su2double* UnitNormal,
                                                   su2double Area, const su2double* GridVel,
                                                   su2double** Jacobian_i, su2double& Res_Conv,
                                                   su2double& Res_Visc) const {

  su2double ProjGridVel = Area * GeometryToolbox::DotProduct(nDim, GridVel, UnitNormal);

  /*--- Retrieve other primitive quantities and viscosities ---*/

  su2double Density = nodes->GetDensity(iPoint);
  su2double Pressure = nodes->GetPressure(iPoint);
  su2double laminar_viscosity = nodes->GetLaminarViscosity(iPoint);
  su2double eddy_viscosity = nodes->GetEddyViscosity(iPoint);
  su2double total_viscosity = laminar_viscosity + eddy_viscosity;

  /*--- Compute the viscous stress tensor ---*/

  su2double tau[MAXNDIM][MAXNDIM] = {{0.0}};
  CNumerics::ComputeStressTensor(nDim, tau, nodes->GetGradient_Primitive(iPoint)+1, total_viscosity);

  /*--- Dot product of the stress tensor with the grid velocity ---*/

  su2double tau_vel[MAXNDIM] = {0.0};
  for (auto iDim = 0u; iDim < nDim; iDim++)
    tau_vel[iDim] = GeometryToolbox::DotProduct(nDim, tau[iDim], GridVel);

  /*--- Compute the convective and viscous residuals (energy eqn.) ---*/

  Res_Conv += Pressure*ProjGridVel;
  Res_Visc += GeometryToolbox::DotProduct(nDim, tau_vel, UnitNormal) * Area;

  /*--- Implicit Jacobian contributions due to moving walls ---*/

  if (Jacobian_i != nullptr) {

    /*--- Jacobian contribution related to the pressure term ---*/

    su2double GridVel2 = GeometryToolbox::SquaredNorm(nDim, GridVel);

    Jacobian_i[nDim+1][0] += 0.5*(Gamma-1.0)*GridVel2*ProjGridVel;

    for (auto jDim = 0u; jDim < nDim; jDim++)
      Jacobian_i[nDim+1][jDim+1] += -(Gamma-1.0)*GridVel[jDim]*ProjGridVel;

    Jacobian_i[nDim+1][nDim+1] += (Gamma-1.0)*ProjGridVel;

    /*--- Now the Jacobian contribution related to the shear stress ---*/

    /*--- Get coordinates of i & nearest normal and compute distance ---*/

    const auto Coord_i = geometry->nodes->GetCoord(iPoint);
    const auto Coord_j = geometry->nodes->GetCoord(Point_Normal);

    su2double dist_ij = GeometryToolbox::Distance(nDim, Coord_i, Coord_j);

    const su2double theta2 = 1.0;

    su2double factor = total_viscosity*Area/(Density*dist_ij);

    if (nDim == 2) {
      su2double thetax = theta2 + UnitNormal[0]*UnitNormal[0]/3.0;
      su2double thetay = theta2 + UnitNormal[1]*UnitNormal[1]/3.0;

      su2double etaz = UnitNormal[0]*UnitNormal[1]/3.0;

      su2double pix = GridVel[0]*thetax + GridVel[1]*etaz;
      su2double piy = GridVel[0]*etaz   + GridVel[1]*thetay;

      Jacobian_i[nDim+1][0] += factor*(-pix*GridVel[0]+piy*GridVel[1]);
      Jacobian_i[nDim+1][1] += factor*pix;
      Jacobian_i[nDim+1][2] += factor*piy;
    }
    else {
      su2double thetax = theta2 + UnitNormal[0]*UnitNormal[0]/3.0;
      su2double thetay = theta2 + UnitNormal[1]*UnitNormal[1]/3.0;
      su2double thetaz = theta2 + UnitNormal[2]*UnitNormal[2]/3.0;

      su2double etaz = UnitNormal[0]*UnitNormal[1]/3.0;
      su2double etax = UnitNormal[1]*UnitNormal[2]/3.0;
      su2double etay = UnitNormal[0]*UnitNormal[2]/3.0;

      su2double pix = GridVel[0]*thetax + GridVel[1]*etaz   + GridVel[2]*etay;
      su2double piy = GridVel[0]*etaz   + GridVel[1]*thetay + GridVel[2]*etax;
      su2double piz = GridVel[0]*etay   + GridVel[1]*etax   + GridVel[2]*thetaz;

      Jacobian_i[nDim+1][0] += factor*(-pix*GridVel[0]+piy*GridVel[1]+piz*GridVel[2]);
      Jacobian_i[nDim+1][1] += factor*pix;
      Jacobian_i[nDim+1][2] += factor*piy;
      Jacobian_i[nDim+1][3] += factor*piz;
    }
  }
}

void CNSSolver::BC_HeatFlux_Wall(CGeometry *geometry, CSolver **solver_container, CNumerics *conv_numerics,
                                 CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {

  /*--- Identify the boundary by string name and get the specified wall
   heat flux from config as well as the wall function treatment. ---*/

  const bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  const auto Marker_Tag = config->GetMarker_All_TagBound(val_marker);
  su2double Wall_HeatFlux = config->GetWall_HeatFlux(Marker_Tag)/config->GetHeat_Flux_Ref();

//  Wall_Function = config->GetWallFunction_Treatment(Marker_Tag);
//  if (Wall_Function != NO_WALL_FUNCTION) {
//    SU2_MPI::Error("Wall function treament not implemented yet", CURRENT_FUNCTION);
//  }

  /*--- Jacobian, initialized to zero if needed. ---*/
  su2double **Jacobian_i = nullptr;
  if (dynamic_grid && implicit) {
    Jacobian_i = new su2double* [nVar];
    for (auto iVar = 0u; iVar < nVar; iVar++)
      Jacobian_i[iVar] = new su2double [nVar] ();
  }

  /*--- Loop over all of the vertices on this boundary marker ---*/

  SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
  for (auto iVertex = 0u; iVertex < geometry->nVertex[val_marker]; iVertex++) {

    const auto iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e, not a halo node) ---*/

    if (!geometry->nodes->GetDomain(iPoint)) continue;

    /*--- If it is a customizable patch, retrieve the specified wall heat flux. ---*/

    if (config->GetMarker_All_PyCustom(val_marker))
      Wall_HeatFlux = geometry->GetCustomBoundaryHeatFlux(val_marker, iVertex);

    /*--- Compute dual-grid area and boundary normal ---*/

    const auto Normal = geometry->vertex[val_marker][iVertex]->GetNormal();

    su2double Area = GeometryToolbox::Norm(nDim, Normal);

    su2double UnitNormal[MAXNDIM] = {0.0};
    for (auto iDim = 0u; iDim < nDim; iDim++)
      UnitNormal[iDim] = -Normal[iDim]/Area;

    /*--- Apply a weak boundary condition for the energy equation.
     Compute the residual due to the prescribed heat flux.
     The convective part will be zero if the grid is not moving. ---*/

    su2double Res_Conv = 0.0;
    su2double Res_Visc = Wall_HeatFlux * Area;

    /*--- Impose the value of the velocity as a strong boundary
     condition (Dirichlet). Fix the velocity and remove any
     contribution to the residual at this node. ---*/

    if (dynamic_grid) {
      nodes->SetVelocity_Old(iPoint, geometry->nodes->GetGridVel(iPoint));
    }
    else {
      su2double zero[MAXNDIM] = {0.0};
      nodes->SetVelocity_Old(iPoint, zero);
    }

    for (auto iDim = 0u; iDim < nDim; iDim++)
      LinSysRes(iPoint, iDim+1) = 0.0;
    nodes->SetVel_ResTruncError_Zero(iPoint);

    /*--- If the wall is moving, there are additional residual contributions
     due to pressure (p v_wall.n) and shear stress (tau.v_wall.n). ---*/

    if (dynamic_grid) {
      if (implicit) {
        for (auto iVar = 0u; iVar < nVar; ++iVar)
          Jacobian_i[nDim+1][iVar] = 0.0;
      }

      const auto Point_Normal = geometry->vertex[val_marker][iVertex]->GetNormal_Neighbor();

      AddDynamicGridResidualContribution(iPoint, Point_Normal, geometry, UnitNormal,
                                         Area, geometry->nodes->GetGridVel(iPoint),
                                         Jacobian_i, Res_Conv, Res_Visc);
    }

    /*--- Convective and viscous contributions to the residual at the wall ---*/

    LinSysRes(iPoint, nDim+1) += Res_Conv - Res_Visc;

    /*--- Enforce the no-slip boundary condition in a strong way by
     modifying the velocity-rows of the Jacobian (1 on the diagonal).
     And add the contributions to the Jacobian due to energy. ---*/

    if (implicit) {
      if (dynamic_grid) {
        Jacobian.AddBlock2Diag(iPoint, Jacobian_i);
      }

      for (auto iVar = 1u; iVar <= nDim; iVar++) {
        auto total_index = iPoint*nVar+iVar;
        Jacobian.DeleteValsRowi(total_index);
      }
    }
  }

  if (Jacobian_i)
    for (auto iVar = 0u; iVar < nVar; iVar++)
      delete [] Jacobian_i[iVar];
  delete [] Jacobian_i;

}

su2double CNSSolver::GetCHTWallTemperature(const CConfig* config, unsigned short val_marker,
                                           unsigned long iVertex, su2double thermal_conductivity,
                                           su2double dist_ij, su2double There,
                                           su2double Temperature_Ref) const {

  /*--- Compute the normal gradient in temperature using Twall ---*/

  const su2double Tconjugate = GetConjugateHeatVariable(val_marker, iVertex, 0) / Temperature_Ref;

  su2double Twall = 0.0;

  if ((config->GetKind_CHT_Coupling() == AVERAGED_TEMPERATURE_NEUMANN_HEATFLUX) ||
      (config->GetKind_CHT_Coupling() == AVERAGED_TEMPERATURE_ROBIN_HEATFLUX)) {

    /*--- Compute wall temperature from both temperatures ---*/

    su2double HF_FactorHere = thermal_conductivity*config->GetViscosity_Ref()/dist_ij;
    su2double HF_FactorConjugate = GetConjugateHeatVariable(val_marker, iVertex, 2);

    Twall = (There*HF_FactorHere + Tconjugate*HF_FactorConjugate)/(HF_FactorHere + HF_FactorConjugate);
  }
  else if ((config->GetKind_CHT_Coupling() == DIRECT_TEMPERATURE_NEUMANN_HEATFLUX) ||
           (config->GetKind_CHT_Coupling() == DIRECT_TEMPERATURE_ROBIN_HEATFLUX)) {

    /*--- (Directly) Set wall temperature to conjugate temperature. ---*/

    Twall = Tconjugate;
  }
  else {
    SU2_MPI::Error("Unknown CHT coupling method.", CURRENT_FUNCTION);
  }

  return Twall;
}

void CNSSolver::BC_Isothermal_Wall_Generic(CGeometry *geometry, CSolver **solver_container,
                                           CNumerics *conv_numerics, CNumerics *visc_numerics,
                                           CConfig *config, unsigned short val_marker, bool cht_mode) {

  const bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  const su2double Temperature_Ref = config->GetTemperature_Ref();
  const su2double Prandtl_Lam = config->GetPrandtl_Lam();
  const su2double Prandtl_Turb = config->GetPrandtl_Turb();
  const su2double Gas_Constant = config->GetGas_ConstantND();
  const su2double Cp = (Gamma / Gamma_Minus_One) * Gas_Constant;

  /*--- Identify the boundary and retrieve the specified wall temperature from
   the config (for non-CHT problems) as well as the wall function treatment. ---*/

  const auto Marker_Tag = config->GetMarker_All_TagBound(val_marker);
  su2double Twall = 0.0;
  if (!cht_mode) {
    Twall = config->GetIsothermal_Temperature(Marker_Tag) / Temperature_Ref;
  }

//  Wall_Function = config->GetWallFunction_Treatment(Marker_Tag);
//  if (Wall_Function != NO_WALL_FUNCTION) {
//    SU2_MPI::Error("Wall function treament not implemented yet", CURRENT_FUNCTION);
//  }

  su2double **Jacobian_i = nullptr;
  if (implicit) {
    Jacobian_i = new su2double* [nVar];
    for (auto iVar = 0u; iVar < nVar; iVar++)
      Jacobian_i[iVar] = new su2double [nVar] ();
  }

  /*--- Loop over boundary points ---*/

  SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
  for (auto iVertex = 0u; iVertex < geometry->nVertex[val_marker]; iVertex++) {

    const auto iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    if (!geometry->nodes->GetDomain(iPoint)) continue;

    /*--- Compute dual-grid area and boundary normal ---*/

    const auto Normal = geometry->vertex[val_marker][iVertex]->GetNormal();

    su2double Area = GeometryToolbox::Norm(nDim, Normal);

    su2double UnitNormal[MAXNDIM] = {0.0};
    for (auto iDim = 0u; iDim < nDim; iDim++)
      UnitNormal[iDim] = -Normal[iDim]/Area;

    /*--- Compute closest normal neighbor ---*/

    const auto Point_Normal = geometry->vertex[val_marker][iVertex]->GetNormal_Neighbor();

    /*--- Get coordinates of i & nearest normal and compute distance ---*/

    const auto Coord_i = geometry->nodes->GetCoord(iPoint);
    const auto Coord_j = geometry->nodes->GetCoord(Point_Normal);

    su2double dist_ij = GeometryToolbox::Distance(nDim, Coord_i, Coord_j);

    /*--- Store the corrected velocity at the wall which will
     be zero (v = 0), unless there is grid motion (v = u_wall)---*/

    if (dynamic_grid) {
      nodes->SetVelocity_Old(iPoint, geometry->nodes->GetGridVel(iPoint));
    }
    else {
      su2double zero[MAXNDIM] = {0.0};
      nodes->SetVelocity_Old(iPoint, zero);
    }

    for (auto iDim = 0u; iDim < nDim; iDim++)
      LinSysRes(iPoint, iDim+1) = 0.0;
    nodes->SetVel_ResTruncError_Zero(iPoint);

    /*--- Get transport coefficients ---*/

    su2double laminar_viscosity    = nodes->GetLaminarViscosity(iPoint);
    su2double eddy_viscosity       = nodes->GetEddyViscosity(iPoint);
    su2double thermal_conductivity = Cp * (laminar_viscosity/Prandtl_Lam + eddy_viscosity/Prandtl_Turb);

    // work in progress on real-gases...
    //thermal_conductivity = nodes->GetThermalConductivity(iPoint);
    //Cp = nodes->GetSpecificHeatCp(iPoint);
    //thermal_conductivity += Cp*eddy_viscosity/Prandtl_Turb;

    /*--- If it is a customizable or CHT patch, retrieve the specified wall temperature. ---*/

    const su2double There = nodes->GetTemperature(Point_Normal);

    if (cht_mode) {
      Twall = GetCHTWallTemperature(config, val_marker, iVertex, dist_ij,
                                    thermal_conductivity, There, Temperature_Ref);
    }
    else if (config->GetMarker_All_PyCustom(val_marker)) {
      Twall = geometry->GetCustomBoundaryTemperature(val_marker, iVertex);
    }

    /*--- Compute the normal gradient in temperature using Twall ---*/

    su2double dTdn = -(There - Twall)/dist_ij;

    /*--- Apply a weak boundary condition for the energy equation.
     Compute the residual due to the prescribed heat flux. ---*/

    su2double Res_Conv = 0.0;
    su2double Res_Visc = thermal_conductivity * dTdn * Area;

    /*--- Calculate Jacobian for implicit time stepping ---*/

    if (implicit) {

      /*--- Add contributions to the Jacobian from the weak enforcement of the energy equations. ---*/

      su2double Density = nodes->GetDensity(iPoint);
      su2double Vel2 = GeometryToolbox::SquaredNorm(nDim, &nodes->GetPrimitive(iPoint)[1]);
      su2double dTdrho = 1.0/Density * ( -Twall + (Gamma-1.0)/Gas_Constant*(Vel2/2.0) );

      Jacobian_i[nDim+1][0] = thermal_conductivity/dist_ij * dTdrho * Area;

      for (auto jDim = 0u; jDim < nDim; jDim++)
        Jacobian_i[nDim+1][jDim+1] = 0.0;

      Jacobian_i[nDim+1][nDim+1] = thermal_conductivity/dist_ij * (Gamma-1.0)/(Gas_Constant*Density) * Area;
    }

    /*--- If the wall is moving, there are additional residual contributions
     due to pressure (p v_wall.n) and shear stress (tau.v_wall.n). ---*/

    if (dynamic_grid) {
      AddDynamicGridResidualContribution(iPoint, Point_Normal, geometry, UnitNormal,
                                         Area, geometry->nodes->GetGridVel(iPoint),
                                         Jacobian_i, Res_Conv, Res_Visc);
    }

    /*--- Convective and viscous contributions to the residual at the wall ---*/

    LinSysRes(iPoint, nDim+1) += Res_Conv - Res_Visc;

    /*--- Enforce the no-slip boundary condition in a strong way by
     modifying the velocity-rows of the Jacobian (1 on the diagonal).
     And add the contributions to the Jacobian due to energy. ---*/

    if (implicit) {
      Jacobian.AddBlock2Diag(iPoint, Jacobian_i);

      for (auto iVar = 1u; iVar <= nDim; iVar++) {
        auto total_index = iPoint*nVar+iVar;
        Jacobian.DeleteValsRowi(total_index);
      }
    }
  }

  if (Jacobian_i)
    for (auto iVar = 0u; iVar < nVar; iVar++)
      delete [] Jacobian_i[iVar];
  delete [] Jacobian_i;

}

void CNSSolver::BC_Isothermal_Wall(CGeometry *geometry, CSolver **solver_container, CNumerics *conv_numerics,
                                   CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {

  BC_Isothermal_Wall_Generic(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
}

void CNSSolver::BC_ConjugateHeat_Interface(CGeometry *geometry, CSolver **solver_container, CNumerics *conv_numerics,
                                           CConfig *config, unsigned short val_marker) {

  BC_Isothermal_Wall_Generic(geometry, solver_container, conv_numerics, nullptr, config, val_marker, true);
}

void CNSSolver::SetTauWall_WF(CGeometry *geometry, CSolver **solver_container, CConfig *config) {

  const su2double Gas_Constant = config->GetGas_ConstantND();
  const su2double Cp = (Gamma / Gamma_Minus_One) * Gas_Constant;

  constexpr unsigned short max_iter = 10;
  const su2double tol = 1e-6;
  const su2double relax = 0.25;

  /*--- Compute the recovery factor ---*/
  // Double-check: laminar or turbulent Pr for this?
  const su2double Recovery = pow(config->GetPrandtl_Lam(), (1.0/3.0));

  /*--- Typical constants from boundary layer theory ---*/

  const su2double kappa = 0.4;
  const su2double B = 5.5;

  for (auto iMarker = 0u; iMarker < config->GetnMarker_All(); iMarker++) {

    if (!config->GetViscous_Wall(iMarker)) continue;

    /*--- Identify the boundary by string name ---*/

    const auto Marker_Tag = config->GetMarker_All_TagBound(iMarker);

    /*--- Get the specified wall heat flux from config ---*/

    // Wall_HeatFlux = config->GetWall_HeatFlux(Marker_Tag);

    /*--- Loop over all of the vertices on this boundary marker ---*/

    SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
    for (auto iVertex = 0u; iVertex < geometry->nVertex[iMarker]; iVertex++) {

      const auto iPoint = geometry->vertex[iMarker][iVertex]->GetNode();
      const auto Point_Normal = geometry->vertex[iMarker][iVertex]->GetNormal_Neighbor();

      /*--- Check if the node belongs to the domain (i.e, not a halo node)
       and the neighbor is not part of the physical boundary ---*/

      if (!geometry->nodes->GetDomain(iPoint)) continue;

      /*--- Get coordinates of the current vertex and nearest normal point ---*/

      const auto Coord = geometry->nodes->GetCoord(iPoint);
      const auto Coord_Normal = geometry->nodes->GetCoord(Point_Normal);

      /*--- Compute dual-grid area and boundary normal ---*/

      const auto Normal = geometry->vertex[iMarker][iVertex]->GetNormal();

      su2double Area = GeometryToolbox::Norm(nDim, Normal);

      su2double UnitNormal[MAXNDIM] = {0.0};
      for (auto iDim = 0u; iDim < nDim; iDim++)
        UnitNormal[iDim] = -Normal[iDim]/Area;

      /*--- Get the velocity, pressure, and temperature at the nearest
       (normal) interior point. ---*/

      su2double Vel[MAXNDIM] = {0.0};
      for (auto iDim = 0u; iDim < nDim; iDim++)
        Vel[iDim] = nodes->GetVelocity(Point_Normal,iDim);
      su2double P_Normal = nodes->GetPressure(Point_Normal);
      su2double T_Normal = nodes->GetTemperature(Point_Normal);

      /*--- Compute the wall-parallel velocity at first point off the wall ---*/

      su2double VelNormal = GeometryToolbox::DotProduct(nDim, Vel, UnitNormal);

      su2double VelTang[MAXNDIM] = {0.0};
      for (auto iDim = 0u; iDim < nDim; iDim++)
        VelTang[iDim] = Vel[iDim] - VelNormal*UnitNormal[iDim];

      su2double VelTangMod = GeometryToolbox::Norm(int(MAXNDIM), VelTang);

      /*--- Compute normal distance of the interior point from the wall ---*/

      su2double WallDist[MAXNDIM] = {0.0};
      GeometryToolbox::Distance(nDim, Coord, Coord_Normal, WallDist);

      su2double WallDistMod = GeometryToolbox::Norm(int(MAXNDIM), WallDist);

      /*--- Compute mach number ---*/

      // M_Normal = VelTangMod / sqrt(Gamma * Gas_Constant * T_Normal);

      /*--- Compute the wall temperature using the Crocco-Buseman equation ---*/

      //T_Wall = T_Normal * (1.0 + 0.5*Gamma_Minus_One*Recovery*M_Normal*M_Normal);
      su2double T_Wall = T_Normal + Recovery*pow(VelTangMod,2.0)/(2.0*Cp);

      /*--- Extrapolate the pressure from the interior & compute the
       wall density using the equation of state ---*/

      su2double P_Wall = P_Normal;
      su2double Density_Wall = P_Wall/(Gas_Constant*T_Wall);

      /*--- Compute the shear stress at the wall in the regular fashion
       by using the stress tensor on the surface ---*/

      su2double tau[MAXNDIM][MAXNDIM] = {{0.0}}, TauElem[MAXNDIM] = {0.0};
      su2double Lam_Visc_Wall = nodes->GetLaminarViscosity(iPoint);
      CNumerics::ComputeStressTensor(nDim, tau, nodes->GetGradient_Primitive(iPoint)+1, Lam_Visc_Wall);

      for (auto iDim = 0u; iDim < nDim; iDim++) {
        TauElem[iDim] = GeometryToolbox::DotProduct(nDim, tau[iDim], UnitNormal);
      }

      /*--- Compute wall shear stress as the magnitude of the wall-tangential
       component of the shear stress tensor---*/

      su2double TauNormal = GeometryToolbox::DotProduct(nDim, TauElem, UnitNormal);

      su2double TauTangent[MAXNDIM] = {0.0};
      for (auto iDim = 0u; iDim < nDim; iDim++)
        TauTangent[iDim] = TauElem[iDim] - TauNormal * UnitNormal[iDim];

      su2double Tau_Wall = GeometryToolbox::Norm(int(MAXNDIM), TauTangent);

      /*--- Calculate the quantities from boundary layer theory and
       iteratively solve for a new wall shear stress. Use the current wall
       shear stress as a starting guess for the wall function. ---*/

      su2double Tau_Wall_Old = Tau_Wall;
      unsigned short counter = 0;
      su2double diff = 1.0;

      while (diff > tol) {

        /*--- Friction velocity and u+ ---*/

        su2double U_Tau = sqrt(Tau_Wall_Old/Density_Wall);
        su2double U_Plus = VelTangMod/U_Tau;

        /*--- Gamma, Beta, Q, and Phi, defined by Nichols & Nelson (2004) ---*/

        su2double Gam  = Recovery*pow(U_Tau,2)/(2.0*Cp*T_Wall);
        su2double Beta = 0.0; // For adiabatic flows only
        su2double Q    = sqrt(Beta*Beta + 4.0*Gam);
        su2double Phi  = asin(-1.0*Beta/Q);

        /*--- Y+ defined by White & Christoph (compressibility and heat transfer)
         negative value for (2.0*Gam*U_Plus - Beta)/Q ---*/

        su2double Y_Plus_White = exp((kappa/sqrt(Gam))*(asin((2.0*Gam*U_Plus - Beta)/Q) - Phi))*exp(-1.0*kappa*B);

        /*--- Spalding's universal form for the BL velocity with the
         outer velocity form of White & Christoph above. ---*/

        su2double kUp = kappa*U_Plus;
        su2double Y_Plus = U_Plus + Y_Plus_White - exp(-1.0*kappa*B) * (1.0 + kUp*(1.0 + 0.5*kUp + pow(kUp,2)/6.0));

        /*--- Calculate an updated value for the wall shear stress using the y+ value,
         the definition of y+, and the definition of the friction velocity. ---*/

        Tau_Wall = (1.0/Density_Wall)*pow(Y_Plus*Lam_Visc_Wall/WallDistMod,2.0);

        /*--- Difference between the old and new Tau. Update old value. ---*/

        diff = fabs(Tau_Wall-Tau_Wall_Old);
        Tau_Wall_Old += relax * (Tau_Wall-Tau_Wall_Old);

        counter++;
        if (counter > max_iter) {
          cout << "WARNING: Tau_Wall evaluation has not converged in CNSSolver.cpp" << endl;
          cout << Tau_Wall_Old << " " << Tau_Wall << " " << diff << endl;
          break;
        }
      }

      /*--- Store this value for the wall shear stress at the node.  ---*/

      nodes->SetTauWall(iPoint, Tau_Wall);

    }

  }

}
