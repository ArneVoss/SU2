/*!
 * \file driver_adjoint_singlezone.cpp
 * \brief The main subroutines for driving adjoint single-zone problems.
 * \author R. Sanchez
 * \version 7.2.0 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2021, SU2 Contributors (cf. AUTHORS.md)
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

#include "../../include/drivers/CDiscAdjSinglezoneDriver.hpp"
#include "../../include/output/tools/CWindowingTools.hpp"
#include "../../include/output/COutputFactory.hpp"
#include "../../include/output/COutputLegacy.hpp"
#include "../../include/output/COutput.hpp"
#include "../../include/iteration/CIterationFactory.hpp"
#include "../../include/iteration/CTurboIteration.hpp"
#include "../../../Common/include/toolboxes/CQuasiNewtonInvLeastSquares.hpp"

CDiscAdjSinglezoneDriver::CDiscAdjSinglezoneDriver(char* confFile,
                                                   unsigned short val_nZone,
                                                   SU2_Comm MPICommunicator) : CSinglezoneDriver(confFile,
                                                                                                 val_nZone,
                                                                                                 MPICommunicator) {


  /*--- Store the number of internal iterations that will be run by the adjoint solver ---*/
  nAdjoint_Iter = config_container[ZONE_0]->GetnInner_Iter();

  /*--- Store the pointers ---*/
  config      = config_container[ZONE_0];
  iteration   = iteration_container[ZONE_0][INST_0];
  solver      = solver_container[ZONE_0][INST_0][MESH_0];
  numerics    = numerics_container[ZONE_0][INST_0][MESH_0];
  geometry    = geometry_container[ZONE_0][INST_0][MESH_0];
  integration = integration_container[ZONE_0][INST_0];

  /*--- Store the recording state ---*/
  RecordingState = RECORDING::CLEAR_INDICES;

  /*--- Initialize the direct iteration ---*/

  switch (config->GetKind_Solver()) {

  case DISC_ADJ_EULER: case DISC_ADJ_NAVIER_STOKES: case DISC_ADJ_RANS:
  case DISC_ADJ_INC_EULER: case DISC_ADJ_INC_NAVIER_STOKES: case DISC_ADJ_INC_RANS:

    if (rank == MASTER_NODE)
      cout << "Direct iteration: Euler/Navier-Stokes/RANS equation." << endl;

    if (config->GetBoolTurbomachinery()) {
      direct_iteration = new CTurboIteration(config);
      output_legacy = COutputFactory::CreateLegacyOutput(config_container[ZONE_0]);
    }
    else { direct_iteration = CIterationFactory::CreateIteration(EULER, config); }

    if (config->GetKind_Regime() == ENUM_REGIME::COMPRESSIBLE) {
      direct_output = COutputFactory::CreateOutput(EULER, config, nDim);
    }
    else { direct_output =  COutputFactory::CreateOutput(INC_EULER, config, nDim); }

    MainVariables = RECORDING::SOLUTION_VARIABLES;
    if (config->GetDeform_Mesh()) {
      SecondaryVariables = RECORDING::MESH_DEFORM;
    }
    else { SecondaryVariables = RECORDING::MESH_COORDS; }
    MainSolver = ADJFLOW_SOL;
    break;

  case DISC_ADJ_NEMO_EULER: case DISC_ADJ_NEMO_NAVIER_STOKES:

    if (rank == MASTER_NODE)
      cout << "Direct iteration: Two-temperature Euler/Navier-Stokes/RANS equation." << endl;

    direct_iteration = CIterationFactory::CreateIteration(NEMO_EULER, config);
    direct_output = COutputFactory::CreateOutput(NEMO_EULER, config, nDim);

    MainVariables = RECORDING::SOLUTION_VARIABLES;
    if (config->GetDeform_Mesh()) {
      SecondaryVariables = RECORDING::MESH_DEFORM;
    }
    else { SecondaryVariables = RECORDING::MESH_COORDS; }
    MainSolver = ADJFLOW_SOL;
    break;

  case DISC_ADJ_FEM_EULER : case DISC_ADJ_FEM_NS : case DISC_ADJ_FEM_RANS :
    if (rank == MASTER_NODE)
      cout << "Direct iteration: Euler/Navier-Stokes/RANS equation." << endl;
    direct_iteration = CIterationFactory::CreateIteration(FEM_EULER, config);
    direct_output = COutputFactory::CreateOutput(FEM_EULER, config, nDim);
    MainVariables = RECORDING::SOLUTION_VARIABLES;
    SecondaryVariables = RECORDING::MESH_COORDS;
    MainSolver = ADJFLOW_SOL;
    break;

  case DISC_ADJ_FEM:
    if (rank == MASTER_NODE)
      cout << "Direct iteration: elasticity equation." << endl;
    direct_iteration =  CIterationFactory::CreateIteration(FEM_ELASTICITY, config);
    direct_output = COutputFactory::CreateOutput(FEM_ELASTICITY, config, nDim);
    MainVariables = RECORDING::SOLUTION_VARIABLES;
    SecondaryVariables = RECORDING::MESH_COORDS;
    MainSolver = ADJFEA_SOL;
    break;

  case DISC_ADJ_HEAT:
    if (rank == MASTER_NODE)
      cout << "Direct iteration: heat equation." << endl;
    direct_iteration = CIterationFactory::CreateIteration(HEAT_EQUATION, config);
    direct_output = COutputFactory::CreateOutput(HEAT_EQUATION, config, nDim);
    MainVariables = RECORDING::SOLUTION_VARIABLES;
    SecondaryVariables = RECORDING::MESH_COORDS;
    MainSolver = ADJHEAT_SOL;
    break;

  }

 direct_output->PreprocessHistoryOutput(config, false);

}

CDiscAdjSinglezoneDriver::~CDiscAdjSinglezoneDriver(void) {

  delete direct_iteration;
  delete direct_output;

}

void CDiscAdjSinglezoneDriver::Preprocess(unsigned long TimeIter) {

  config_container[ZONE_0]->SetTimeIter(TimeIter);

  /*--- Preprocess the adjoint iteration ---*/

  iteration->Preprocess(output_container[ZONE_0], integration_container, geometry_container,
                        solver_container, numerics_container, config_container,
                        surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);

  /*--- For the adjoint iteration we need the derivatives of the iteration function with
   *--- respect to the conservative variables. Since these derivatives do not change in the steady state case
   *--- we only have to record if the current recording is different from the main variables. ---*/

  if (RecordingState != MainVariables){
    MainRecording();
  }

}

void CDiscAdjSinglezoneDriver::Run() {

    if (config->GetKind_DiscreteAdjoint() == ENUM_DISC_ADJ_TYPE::RESIDUALS) {
        Run_Residual();
    } else {
        Run_FixedPoint();
    }
}

void CDiscAdjSinglezoneDriver::Run_FixedPoint() {

  CQuasiNewtonInvLeastSquares<passivedouble> fixPtCorrector;
  if (config->GetnQuasiNewtonSamples() > 1) {
    fixPtCorrector.resize(config->GetnQuasiNewtonSamples(),
                          geometry_container[ZONE_0][INST_0][MESH_0]->GetnPoint(),
                          GetTotalNumberOfVariables(ZONE_0,true),
                          geometry_container[ZONE_0][INST_0][MESH_0]->GetnPointDomain());

    if (TimeIter != 0) GetAllSolutions(ZONE_0, true, fixPtCorrector);
  }

  for (auto Adjoint_Iter = 0ul; Adjoint_Iter < nAdjoint_Iter; Adjoint_Iter++) {

    /*--- Initialize the adjoint of the output variables of the iteration with the adjoint solution
     *--- of the previous iteration. The values are passed to the AD tool.
     *--- Issues with iteration number should be dealt with once the output structure is in place. ---*/

    config->SetInnerIter(Adjoint_Iter);

    iteration->InitializeAdjoint(solver_container, geometry_container, config_container, ZONE_0, INST_0);

    /*--- Initialize the adjoint of the objective function with 1.0. ---*/

    SetAdj_ObjFunction();

    /*--- Interpret the stored information by calling the corresponding routine of the AD tool. ---*/

    AD::ComputeAdjoint();

    /*--- Extract the computed adjoint values of the input variables and store them for the next iteration. ---*/

    iteration->IterateDiscAdj(geometry_container, solver_container,
                              config_container, ZONE_0, INST_0, false);

    /*--- Monitor the pseudo-time ---*/

    StopCalc = iteration->Monitor(output_container[ZONE_0], integration_container, geometry_container,
                                  solver_container, numerics_container, config_container,
                                  surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);

    /*--- Clear the stored adjoint information to be ready for a new evaluation. ---*/

    AD::ClearAdjoints();

    /*--- Output files for steady state simulations. ---*/

    if (!config->GetTime_Domain()) {
      iteration->Output(output_container[ZONE_0], geometry_container, solver_container,
                        config_container, Adjoint_Iter, false, ZONE_0, INST_0);
    }

    if (StopCalc) break;

    /*--- Correct the solution with the quasi-Newton approach. ---*/

    if (fixPtCorrector.size()) {
      GetAllSolutions(ZONE_0, true, fixPtCorrector.FPresult());
      SetAllSolutions(ZONE_0, true, fixPtCorrector.compute());
    }

  }
}

void CDiscAdjSinglezoneDriver::Run_Residual() {

    /*--- Initialize the adjoint of the output variables of the iteration with the adjoint solution
     *--- of the previous iteration. The values are passed to the AD tool.
     *--- Issues with iteration number should be dealt with once the output structure is in place. ---*/

    iteration->InitializeAdjoint(solver_container, geometry_container, config_container, ZONE_0, INST_0);

    /*--- Interpret the stored information by calling the corresponding routine of the AD tool. ---*/

    AD::ComputeAdjoint();

    /*--- Extract the adjoints of the residuals and store them for the next iteration ---*/

    if (config->GetFluidProblem()) {
        solver[ADJFLOW_SOL]->ExtractAdjoint_Solution_Residual(geometry, config, ENUM_VARIABLE::RESIDUALS);

        solver[ADJFLOW_SOL]->ExtractAdjoint_Variables_Residual(geometry, config, ENUM_VARIABLE::RESIDUALS);
    }

    /*--- Clear the stored adjoint information to be ready for a new evaluation. ---*/

    AD::ClearAdjoints();

    /*--- Initialize the adjoint of the objective function with 1.0. ---*/

    SetAdj_ObjFunction();

    /*--- Interpret the stored information by calling the corresponding routine of the AD tool. ---*/

    AD::ComputeAdjoint();

    /*--- Extract the adjoints of the objective function and store them for the next iteration ---*/

    if (config->GetFluidProblem()) {
        solver[ADJFLOW_SOL]->ExtractAdjoint_Solution_Residual(geometry, config, ENUM_VARIABLE::OBJECTIVE);

        solver[ADJFLOW_SOL]->ExtractAdjoint_Variables_Residual(geometry, config, ENUM_VARIABLE::OBJECTIVE);
    }

    /*--- Clear the stored adjoint information to be ready for a new evaluation. ---*/

    AD::ClearAdjoints();

    /*--- Initialize the adjoint of the vertex tractions with the corresponding adjoint vector. ---*/

    solver[FLOW_SOL]->SetVertexTractionsAdjoint(geometry, config);

    /*--- Interpret the stored information by calling the corresponding routine of the AD tool. ---*/

    AD::ComputeAdjoint();

    /*--- Extract the adjoints of the vertex tractions and store them for the next iteration ---*/

    if (config->GetFluidProblem()) {
        solver[ADJFLOW_SOL]->ExtractAdjoint_Solution_Residual(geometry, config, ENUM_VARIABLE::TRACTIONS);
    }

    /*--- Clear the stored adjoint information to be ready for a new evaluation. ---*/

    AD::ClearAdjoints();

}

void CDiscAdjSinglezoneDriver::Postprocess() {

  switch(config->GetKind_Solver())
  {
    case DISC_ADJ_EULER :     case DISC_ADJ_NAVIER_STOKES :     case DISC_ADJ_RANS :
    case DISC_ADJ_INC_EULER : case DISC_ADJ_INC_NAVIER_STOKES : case DISC_ADJ_INC_RANS :
    case DISC_ADJ_NEMO_EULER: case DISC_ADJ_NEMO_NAVIER_STOKES:
    case DISC_ADJ_HEAT :

      /*--- Compute the geometrical sensitivities ---*/
      SecondaryRecording();

      if (config->GetKind_DiscreteAdjoint() == ENUM_DISC_ADJ_TYPE::RESIDUALS) {
          SecondaryRun_Residual();
      } else {
          SecondaryRun_FixedPoint();
      }

      break;

    case DISC_ADJ_FEM :

      /*--- Compute the geometrical sensitivities ---*/
      SecondaryRecording();

      if (config->GetKind_DiscreteAdjoint() == ENUM_DISC_ADJ_TYPE::RESIDUALS) {
          SecondaryRun_Residual();
      } else {
          SecondaryRun_FixedPoint();
      }

      iteration->Postprocess(output_container[ZONE_0], integration_container, geometry_container,
                             solver_container, numerics_container, config_container,
                             surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);
      break;
  }//switch

}

void CDiscAdjSinglezoneDriver::SetRecording(RECORDING kind_recording){

  AD::Reset();

  /*--- Prepare for recording by resetting the solution to the initial converged solution. ---*/

  for (unsigned short iSol=0; iSol < MAX_SOLS; iSol++) {
    for (unsigned short iMesh = 0; iMesh <= config_container[ZONE_0]->GetnMGLevels(); iMesh++) {
      auto solver = solver_container[ZONE_0][INST_0][iMesh][iSol];
      if (solver && solver->GetAdjoint()) {
        solver->SetRecording(geometry_container[ZONE_0][INST_0][iMesh], config_container[ZONE_0]);
      }
    }
  }

  if (rank == MASTER_NODE) {
    cout << "\n-------------------------------------------------------------------------\n";
    switch(kind_recording) {
    case RECORDING::CLEAR_INDICES: cout << "Clearing the computational graph." << endl; break;
    case RECORDING::MESH_COORDS:   cout << "Storing computational graph wrt MESH COORDINATES." << endl; break;
    case RECORDING::SOLUTION_VARIABLES:
      cout << "Direct iteration to store the primal computational graph." << endl;
      cout << "Computing residuals to check the convergence of the direct problem." << endl; break;
    default: break;
    }
  }

  /*---Enable recording and register input of the iteration --- */

  if (kind_recording != RECORDING::CLEAR_INDICES){

    AD::StartRecording();

    if (rank == MASTER_NODE && kind_recording == MainVariables) {
      cout << endl << "-------------------------------------------------------------------------" << endl;
      cout << "Direct iteration to store the primal computational graph." << endl;
      cout << "Compute residuals to check the convergence of the direct problem." << endl;
    }

    iteration->RegisterInput(solver_container, geometry_container, config_container, ZONE_0, INST_0, kind_recording);
  }

  /*--- Set the dependencies of the iteration ---*/

  iteration->SetDependencies(solver_container, geometry_container, numerics_container, config_container, ZONE_0,
                             INST_0, kind_recording);

  /*--- Do one iteration of the direct solver ---*/

  if (config->GetKind_DiscreteAdjoint() == ENUM_DISC_ADJ_TYPE::RESIDUALS) {
      DirectRun_Residual(kind_recording);
  }
  else {
      DirectRun_FixedPoint(kind_recording);
  }

  /*--- Store the recording state ---*/

  RecordingState = kind_recording;

  /*--- Register Output of the iteration ---*/

  iteration->RegisterOutput(solver_container, geometry_container, config_container, ZONE_0, INST_0);

  /*--- Extract the objective function and store it --- */

  SetObjFunction();

  if (kind_recording != RECORDING::CLEAR_INDICES && config_container[ZONE_0]->GetWrt_AD_Statistics()) {
    if (rank == MASTER_NODE) AD::PrintStatistics();
#ifdef CODI_REVERSE_TYPE
    if (size > SINGLE_NODE) {
      su2double myMem = AD::getGlobalTape().getTapeValues().getUsedMemorySize(), totMem = 0.0;
      SU2_MPI::Allreduce(&myMem, &totMem, 1, MPI_DOUBLE, MPI_SUM, SU2_MPI::GetComm());
      if (rank == MASTER_NODE) {
        cout << "MPI\n";
        cout << "-------------------------------------\n";
        cout << "  Total memory used      :  " << totMem << " MB\n";
        cout << "-------------------------------------\n" << endl;
      }
    }
#endif
  }

  AD::StopRecording();

}

void CDiscAdjSinglezoneDriver::SetAdj_ObjFunction(){

  const auto IterAvg_Obj = config->GetIter_Avg_Objective();
  su2double seeding = 1.0;

  CWindowingTools windowEvaluator = CWindowingTools();

  if (config->GetTime_Marching() != TIME_MARCHING::STEADY){
    if (TimeIter < IterAvg_Obj){
      /*--- Default behavior (in case no specific window is chosen) is to use Square-Windowing, i.e. the numerator equals 1.0 ---*/
      seeding = windowEvaluator.GetWndWeight(config->GetKindWindow(),TimeIter, IterAvg_Obj-1)/ (static_cast<su2double>(IterAvg_Obj));
    }
    else {
      seeding = 0.0;
    }
  }

  if (rank == MASTER_NODE){
    SU2_TYPE::SetDerivative(ObjFunc, SU2_TYPE::GetValue(seeding));
  } else {
    SU2_TYPE::SetDerivative(ObjFunc, 0.0);
  }
}

void CDiscAdjSinglezoneDriver::SetObjFunction(){

  bool heat         = (config->GetWeakly_Coupled_Heat());
  bool turbo        = (config->GetBoolTurbomachinery());

  ObjFunc = 0.0;

  direct_output->SetHistory_Output(geometry, solver, config,
                                   config->GetTimeIter(),
                                   config->GetOuterIter(),
                                   config->GetInnerIter());

  /*--- Specific scalar objective functions ---*/

  switch (config->GetKind_Solver()) {
  case DISC_ADJ_INC_EULER:       case DISC_ADJ_INC_NAVIER_STOKES:      case DISC_ADJ_INC_RANS:
  case DISC_ADJ_EULER:           case DISC_ADJ_NAVIER_STOKES:          case DISC_ADJ_RANS:
  case DISC_ADJ_FEM_EULER:       case DISC_ADJ_FEM_NS:                 case DISC_ADJ_FEM_RANS:
  case DISC_ADJ_NEMO_EULER:      case DISC_ADJ_NEMO_NAVIER_STOKES:


    solver[FLOW_SOL]->SetTotal_ComboObj(0.0);

    /*--- Surface based obj. function ---*/

    solver[FLOW_SOL]->Evaluate_ObjFunc(config);
    ObjFunc += solver[FLOW_SOL]->GetTotal_ComboObj();
    if (heat){
      if (config->GetKind_ObjFunc() == TOTAL_HEATFLUX) {
        ObjFunc += solver[HEAT_SOL]->GetTotal_HeatFlux();
      }
      else if (config->GetKind_ObjFunc() == AVG_TEMPERATURE) {
        ObjFunc += solver[HEAT_SOL]->GetTotal_AvgTemperature();
      }
    }

    /*--- This calls to be moved to a generic framework at a next stage         ---*/
    /*--- Some things that are currently hacked into output must be reorganized ---*/
    if (turbo){

      solver[FLOW_SOL]->SetTotal_ComboObj(0.0);
      output_legacy->ComputeTurboPerformance(solver[FLOW_SOL], geometry, config);

      unsigned short nMarkerTurboPerf = config->GetnMarker_TurboPerformance();
      unsigned short nSpanSections = config->GetnSpanWiseSections();

      switch (config_container[ZONE_0]->GetKind_ObjFunc()){
      case ENTROPY_GENERATION:
        solver[FLOW_SOL]->AddTotal_ComboObj(output_legacy->GetEntropyGen(nMarkerTurboPerf-1, nSpanSections));
        break;
      case FLOW_ANGLE_OUT:
        solver[FLOW_SOL]->AddTotal_ComboObj(output_legacy->GetFlowAngleOut(nMarkerTurboPerf-1, nSpanSections));
        break;
      case MASS_FLOW_IN:
        solver[FLOW_SOL]->AddTotal_ComboObj(output_legacy->GetMassFlowIn(nMarkerTurboPerf-1, nSpanSections));
        break;
      default:
        break;
      }

      ObjFunc = solver[FLOW_SOL]->GetTotal_ComboObj();

    }
    break;

  case DISC_ADJ_HEAT:
    switch (config->GetKind_ObjFunc()){
    case TOTAL_HEATFLUX:
      ObjFunc = solver[HEAT_SOL]->GetTotal_HeatFlux();
      break;
    case AVG_TEMPERATURE:
      ObjFunc = solver[HEAT_SOL]->GetTotal_AvgTemperature();
      break;
    default:
      break;
    }
    break;

  case DISC_ADJ_FEM:
    solver[FEA_SOL]->Postprocessing(geometry, config, numerics_container[ZONE_0][INST_0][MESH_0][FEA_SOL], true);
    solver[FEA_SOL]->Evaluate_ObjFunc(config);
    ObjFunc = solver[FEA_SOL]->GetTotal_ComboObj();
    break;
  }

  if (rank == MASTER_NODE){
    AD::RegisterOutput(ObjFunc);
  }

}

void CDiscAdjSinglezoneDriver::DirectRun_FixedPoint(RECORDING kind_recording) {

  /*--- Mesh movement ---*/

  direct_iteration->SetMesh_Deformation(geometry_container[ZONE_0][INST_0], solver, numerics, config, kind_recording);

  /*--- Zone preprocessing ---*/

  direct_iteration->Preprocess(direct_output, integration_container, geometry_container, solver_container, numerics_container, config_container, surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);

  /*--- Iterate the direct solver ---*/

  direct_iteration->Iterate(direct_output, integration_container, geometry_container, solver_container, numerics_container, config_container, surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);

  /*--- Postprocess the direct solver ---*/

  direct_iteration->Postprocess(direct_output, integration_container, geometry_container, solver_container, numerics_container, config_container, surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);

  /*--- Print the direct residual to screen ---*/

  Print_DirectResidual(kind_recording);

}

void CDiscAdjSinglezoneDriver::DirectRun_Residual(RECORDING kind_recording) {

    /*--- Mesh movement ---*/

    direct_iteration->SetMesh_Deformation(geometry_container[ZONE_0][INST_0], solver, numerics, config, kind_recording);

    /*--- Residuals calculation ---*/

    Update_DirectSolution();

    /*--- Print the direct residual to screen ---*/

    Print_DirectResidual(kind_recording);
}

void CDiscAdjSinglezoneDriver::Print_DirectResidual(RECORDING kind_recording){

  /*--- Print the residuals of the direct iteration that we just recorded ---*/
  /*--- This routine should be moved to the output, once the new structure is in place ---*/
  if ((rank == MASTER_NODE) && (kind_recording == MainVariables)){

    switch (config->GetKind_Solver()) {

    case DISC_ADJ_EULER: case DISC_ADJ_NAVIER_STOKES: case DISC_ADJ_RANS:
    case DISC_ADJ_INC_EULER: case DISC_ADJ_INC_NAVIER_STOKES: case DISC_ADJ_INC_RANS:
    case DISC_ADJ_FEM_EULER : case DISC_ADJ_FEM_NS : case DISC_ADJ_FEM_RANS :
    case DISC_ADJ_NEMO_EULER: case DISC_ADJ_NEMO_NAVIER_STOKES: 
      cout << "log10[U(0)]: "   << log10(solver[FLOW_SOL]->GetRes_RMS(0))
           << ", log10[U(1)]: " << log10(solver[FLOW_SOL]->GetRes_RMS(1))
           << ", log10[U(2)]: " << log10(solver[FLOW_SOL]->GetRes_RMS(2)) << "." << endl;
      cout << "log10[U(3)]: " << log10(solver[FLOW_SOL]->GetRes_RMS(3));
      if (geometry->GetnDim() == 3) cout << ", log10[U(4)]: " << log10(solver[FLOW_SOL]->GetRes_RMS(4));
      cout << "." << endl;
      if ( config->GetKind_Turb_Model() != NONE && !config->GetFrozen_Visc_Disc()) {
        cout << "log10[Turb(0)]: "   << log10(solver[TURB_SOL]->GetRes_RMS(0));
        if (solver[TURB_SOL]->GetnVar() > 1) cout << ", log10[Turb(1)]: " << log10(solver[TURB_SOL]->GetRes_RMS(1));
        cout << "." << endl;
      }
      if (config->GetWeakly_Coupled_Heat()){
        cout << "log10[Heat(0)]: "   << log10(solver[HEAT_SOL]->GetRes_RMS(0)) << "." << endl;
      }
      if ( config->AddRadiation()) {
        cout <<"log10[E(rad)]: " << log10(solver[RAD_SOL]->GetRes_RMS(0)) << endl;
      }
      break;

    case DISC_ADJ_FEM:

      if (config->GetGeometricConditions() == STRUCT_DEFORMATION::LARGE){
        cout << "UTOL-A: "   << log10(solver[FEA_SOL]->GetRes_FEM(0))
             << ", RTOL-A: " << log10(solver[FEA_SOL]->GetRes_FEM(1))
             << ", ETOL-A: " << log10(solver[FEA_SOL]->GetRes_FEM(2)) << "." << endl;
      }
      else{
        if (geometry->GetnDim() == 2){
          cout << "log10[RMS Ux]: "   << log10(solver[FEA_SOL]->GetRes_RMS(0))
               << ", log10[RMS Uy]: " << log10(solver[FEA_SOL]->GetRes_RMS(1)) << "." << endl;
        }
        else{
          cout << "log10[RMS Ux]: "   << log10(solver[FEA_SOL]->GetRes_RMS(0))
               << ", log10[RMS Uy]: " << log10(solver[FEA_SOL]->GetRes_RMS(1))
               << ", log10[RMS Uz]: " << log10(solver[FEA_SOL]->GetRes_RMS(2))<< "." << endl;
        }
      }

      break;

    case DISC_ADJ_HEAT:
      cout << "log10[Cons(0)]: "   << log10(solver[HEAT_SOL]->GetRes_RMS(0)) << "." << endl;
      break;

    }

    cout << "-------------------------------------------------------------------------" << endl << endl;
  }
  else if ((rank == MASTER_NODE) && (kind_recording == SecondaryVariables) && (SecondaryVariables != RECORDING::CLEAR_INDICES)){
    cout << endl << "Recording the computational graph with respect to the ";
    switch (SecondaryVariables){
      case RECORDING::MESH_COORDS: cout << "mesh coordinates." << endl;    break;
      default:                     cout << "secondary variables." << endl; break;
     }
  }

}

void CDiscAdjSinglezoneDriver::MainRecording(){

  /*--- SetRecording stores the computational graph on one iteration of the direct problem. Calling it with NONE
   *    as argument ensures that all information from a previous recording is removed. ---*/

  SetRecording(RECORDING::CLEAR_INDICES);

  /*--- Store the computational graph of one direct iteration with the solution variables as input. ---*/

  SetRecording(MainVariables);

}

void CDiscAdjSinglezoneDriver::SecondaryRecording(){

  /*--- SetRecording stores the computational graph on one iteration of the direct problem. Calling it with NONE
   *    as argument ensures that all information from a previous recording is removed. ---*/

  SetRecording(RECORDING::CLEAR_INDICES);

  /*--- Store the computational graph of one direct iteration with the secondary variables as input. ---*/

  SetRecording(SecondaryVariables);

}

void CDiscAdjSinglezoneDriver::SecondaryRun_FixedPoint() {
  /*--- Initialize the adjoint of the output variables of the iteration with the adjoint solution
   *    of the current iteration. The values are passed to the AD tool. ---*/

  iteration->InitializeAdjoint(solver_container, geometry_container, config_container, ZONE_0, INST_0);

  /*--- Initialize the adjoint of the objective function with 1.0. ---*/

  SetAdj_ObjFunction();

  /*--- Interpret the stored information by calling the corresponding routine of the AD tool. ---*/

  AD::ComputeAdjoint();

  /*--- Extract the computed sensitivity values. ---*/

  if (SecondaryVariables == RECORDING::MESH_COORDS) {
    solver[MainSolver]->SetSensitivity(geometry, config);
  }
  else { // MESH_DEFORM
    solver[ADJMESH_SOL]->SetSensitivity(geometry, config, solver[MainSolver]);
  }

  /*--- Clear the stored adjoint information to be ready for a new evaluation. ---*/

  AD::ClearAdjoints();

}

void CDiscAdjSinglezoneDriver::SecondaryRun_Residual() {
    /*--- Initialize the adjoint of the output variables of the iteration with the adjoint solution
     *--- of the previous iteration. The values are passed to the AD tool.
     *--- Issues with iteration number should be dealt with once the output structure is in place. ---*/

    iteration->InitializeAdjoint(solver_container, geometry_container, config_container, ZONE_0, INST_0);

    /*--- Interpret the stored information by calling the corresponding routine of the AD tool. ---*/

    AD::ComputeAdjoint();

    /*--- Extract the adjoints of the residuals and store them for the next iteration ---*/

    if (config->GetFluidProblem()) {
        if (SecondaryVariables == RECORDING::MESH_COORDS) {
            solver[ADJFLOW_SOL]->ExtractAdjoint_Geometry_Residual(geometry, config, nullptr, ENUM_VARIABLE::RESIDUALS);
        }
        else { // MESH_DEFORM
            solver[ADJFLOW_SOL]->ExtractAdjoint_Geometry_Residual(geometry, config, solver[ADJMESH_SOL], ENUM_VARIABLE::RESIDUALS);
        }
    }

    /*--- Clear the stored adjoint information to be ready for a new evaluation. ---*/

    AD::ClearAdjoints();

    /*--- Initialize the adjoint of the objective function with 1.0. ---*/

    SetAdj_ObjFunction();

    /*--- Interpret the stored information by calling the corresponding routine of the AD tool. ---*/

    AD::ComputeAdjoint();

    /*--- Extract the adjoints of the objective function and store them for the next iteration ---*/

    if (config->GetFluidProblem()) {
        if (SecondaryVariables == RECORDING::MESH_COORDS) {
            solver[ADJFLOW_SOL]->ExtractAdjoint_Geometry_Residual(geometry, config, nullptr, ENUM_VARIABLE::OBJECTIVE);
        }
        else { // MESH_DEFORM
            solver[ADJMESH_SOL]->ExtractAdjoint_Geometry_Residual(geometry, config, solver[ADJFLOW_SOL], ENUM_VARIABLE::OBJECTIVE);
        }
    }

    /*--- Clear the stored adjoint information to be ready for a new evaluation. ---*/

    AD::ClearAdjoints();

    /*--- Initialize the adjoint of the vertex tractions with the corresponding adjoint vector. ---*/

    solver[FLOW_SOL]->SetVertexTractionsAdjoint(geometry, config);

    /*--- Interpret the stored information by calling the corresponding routine of the AD tool. ---*/

    AD::ComputeAdjoint();

    /*--- Extract the adjoints of the vertex tractions and store them for the next iteration ---*/

    if (config->GetFluidProblem()) {
        if (SecondaryVariables == RECORDING::MESH_COORDS) {
            solver[ADJFLOW_SOL]->ExtractAdjoint_Geometry_Residual(geometry, config, nullptr, ENUM_VARIABLE::TRACTIONS);
        }
        else { // MESH_DEFORM
            solver[ADJMESH_SOL]->ExtractAdjoint_Geometry_Residual(geometry, config, solver[ADJFLOW_SOL], ENUM_VARIABLE::TRACTIONS);
        }
    }

    /*--- Clear the stored adjoint information to be ready for a new evaluation. ---*/

    AD::ClearAdjoints();

}

void CDiscAdjSinglezoneDriver::Update_DirectSolution() {

    //  TODO:   Update/set Far-field conditions here instead of in the setter for AoA and Mach
    su2double Velocity_Ref = config->GetVelocity_Ref();
    su2double Alpha                  = config->GetAoA()*PI_NUMBER/180.0;
    su2double Beta                   = config->GetAoS()*PI_NUMBER/180.0;
    su2double Mach                   = config->GetMach();
    su2double Temperature            = config->GetTemperature_FreeStream();
    su2double Temperature_ve         = config->GetTemperature_ve_FreeStream();
    su2double Gas_Constant           = config->GetGas_Constant();
    su2double Gamma                  = config->GetGamma();
    su2double SoundSpeed             = sqrt(Gamma*Gas_Constant*Temperature);

    const su2double *MassFrac = config->GetGas_Composition();
    su2double *Mvec_Inf;
    Mvec_Inf = new su2double[nDim];

    if (nDim == 2) {
        config->GetVelocity_FreeStreamND()[0] = cos(Alpha)*Mach*SoundSpeed/Velocity_Ref;
        config->GetVelocity_FreeStreamND()[1] = sin(Alpha)*Mach*SoundSpeed/Velocity_Ref;

      Mvec_Inf[0] = cos(Alpha)*Mach;
      Mvec_Inf[1] = sin(Alpha)*Mach;
    }
    if (nDim == 3) {
        config->GetVelocity_FreeStreamND()[0] = cos(Alpha)*cos(Beta)*Mach*SoundSpeed/Velocity_Ref;
        config->GetVelocity_FreeStreamND()[1] = sin(Beta)*Mach*SoundSpeed/Velocity_Ref;
        config->GetVelocity_FreeStreamND()[2] = sin(Alpha)*cos(Beta)*Mach*SoundSpeed/Velocity_Ref;

      Mvec_Inf[0] = cos(Alpha)*cos(Beta)*Mach;
      Mvec_Inf[1] = sin(Beta)*Mach;
      Mvec_Inf[2] = sin(Alpha)*cos(Beta)*Mach;

    }

    /*--- Update the dual grid (without multigrid). ---*/
    geometry->InitiateComms(geometry, config, COORDINATES);
    geometry->CompleteComms(geometry, config, COORDINATES);

    geometry->SetControlVolume(config, UPDATE);
    geometry->SetBoundControlVolume(config, UPDATE);
    geometry->SetMaxLength(config);

    /*--- Flow and turbulent conservative state variables ---*/

    direct_iteration->Preprocess(direct_output, integration_container, geometry_container, solver_container, numerics_container, config_container, surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);

    /*--- Solve the Euler, Navier-Stokes or Reynolds-averaged Navier-Stokes (RANS) equations (one iteration) ---*/

    integration[FLOW_SOL]->ComputeResiduals(geometry_container, solver_container, numerics_container, config_container, FLOW_SOL, ZONE_0, INST_0);

    /*--- Flow tractions ---*/

    direct_iteration->Postprocess(direct_output, integration_container, geometry_container, solver_container, numerics_container, config_container, surface_movement, grid_movement, FFDBox, ZONE_0, INST_0);

}
