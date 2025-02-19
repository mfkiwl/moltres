#include "NtAction.h"

// MOOSE includes
#include "Factory.h"
#include "Parser.h"
#include "Conversion.h"
#include "FEProblem.h"
#include "NonlinearSystemBase.h"
#include "InputParameterWarehouse.h"
#include "AddVariableAction.h"

#include "libmesh/enum_to_string.h"

registerMooseAction("MoltresApp", NtAction, "add_kernel");
registerMooseAction("MoltresApp", NtAction, "add_bc");
registerMooseAction("MoltresApp", NtAction, "add_variable");
registerMooseAction("MoltresApp", NtAction, "add_ic");
registerMooseAction("MoltresApp", NtAction, "add_aux_variable");
registerMooseAction("MoltresApp", NtAction, "add_aux_kernel");
registerMooseAction("MoltresApp", NtAction, "check_copy_nodal_vars");
registerMooseAction("MoltresApp", NtAction, "copy_nodal_vars");

InputParameters
NtAction::validParams()
{
  InputParameters params = VariableNotAMooseObjectAction::validParams();

  params.addRequiredParam<unsigned int>("num_precursor_groups",
                                        "specifies the total number of precursors to create");
  params.addRequiredParam<std::string>("var_name_base",
                                       "specifies the base name of the variables");
  params.addRequiredCoupledVar("temperature", "Name of temperature variable");
  params.addCoupledVar("pre_concs",
                       "All the variables that hold the precursor concentrations. "
                       "These MUST be listed by increasing group number.");
  params.addParam<Real>("temp_scaling", "The amount by which to scale the temperature variable.");
  params.addRequiredParam<unsigned int>("num_groups", "The total number of energy groups.");
  params.addRequiredParam<bool>(
      "use_exp_form", "Whether concentrations should be in an exponential/logarithmic format.");
  params.addParam<bool>("jac_test",
                        false,
                        "Whether we're testing the Jacobian and should use some "
                        "random initial conditions for the precursors.");
  params.addParam<FunctionName>("nt_ic_function",
                                "An initial condition function for the neutrons.");
  params.addParam<std::vector<BoundaryName>>(
      "vacuum_boundaries",
      "The boundaries on which to apply vacuum boundaries.");
  MooseEnum vacuum_bc_type("marshak mark milne", "marshak");
  params.addParam<MooseEnum>("vacuum_bc_type", vacuum_bc_type,
      "Whether to apply Marshak, Mark, or Milne vacuum boundary conditions. Defaults to Marshak.");
  params.addParam<bool>(
      "create_temperature_var", true, "Whether to create the temperature variable.");
  params.addParam<bool>(
      "init_nts_from_file",
      false,
      "Whether to restart simulation using nt output from a previous simulation.");
  params.addParam<bool>(
      "init_temperature_from_file",
      false,
      "Whether to restart simulation using temperature output from a previous simulation.");
  params.addParam<bool>(
      "dg_for_temperature",
      true,
      "Whether the temperature variable should use discontinuous basis functions.");
  params.addParam<bool>(
      "eigen", false, "Whether to run an eigen- instead of a transient- simulation.");
  params.addRequiredParam<bool>("account_delayed", "Whether to account for delayed neutrons.");
  params.addRequiredParam<bool>("sss2_input",
                                "Whether the input follows sss2 form scattering matrices.");
  params.addParam<std::vector<SubdomainName>>("pre_blocks", "The blocks the precursors live on.");
  params.addParam<Real>("eigenvalue_scaling",
                        1.0,
                        "Artificial scaling factor for the fission source. Primarily for "
                        "introducing artificial reactivity to make super/subcritical systems "
                        "exactly critical or to simulate reactivity insertions/withdrawals.");
  return params;
}

NtAction::NtAction(const InputParameters & params)
  : VariableNotAMooseObjectAction(params),
    _num_precursor_groups(getParam<unsigned int>("num_precursor_groups")),
    _var_name_base(getParam<std::string>("var_name_base")),
    _num_groups(getParam<unsigned int>("num_groups"))
{
  if (!isParamValid("pre_concs") && getParam<bool>("account_delayed"))
    mooseError("If we're accounting for delayed neutrons, then you must supply 'pre_concs'.");
}

void
NtAction::act()
{
  std::vector<VariableName> all_var_names;
  for (unsigned int op = 1; op <= _num_groups; ++op)
    all_var_names.push_back(_var_name_base + Moose::stringify(op));

  for (unsigned int op = 1; op <= _num_groups; ++op)
  {
    std::string var_name = _var_name_base + Moose::stringify(op);

    //
    // See whether we want to use an old solution
    //
    if (getParam<bool>("init_nts_from_file"))
    {
      if (_current_task == "check_copy_nodal_vars")
        _app.setExodusFileRestart(true);

      if (_current_task == "copy_nodal_vars")
      {
        SystemBase * system = &_problem->getNonlinearSystemBase(/*nl_sys_num=*/0);
        system->addVariableToCopy(var_name, var_name, "LATEST");
      }
    }

    //
    // Create variable names
    //

    if (_current_task == "add_variable")
    {
      addVariable(var_name);
    }

    if (_current_task == "add_kernel")
    {
      // Set up time derivatives
      if (!getParam<bool>("eigen"))
        addNtKernel(op, var_name, "NtTimeDerivative", all_var_names);
      // Set up GroupDiffusion
      addNtKernel(op, var_name, "GroupDiffusion", all_var_names);
      // Set up SigmaR
      addNtKernel(op, var_name, "SigmaR", all_var_names);
      // Set up InScatter
      if (_num_groups != 1)
        addNtKernel(op, var_name, "InScatter", all_var_names);
      // Set up CoupledFissionKernel
      addCoupledFissionKernel(op, var_name, all_var_names);
      // Set up DelayedNeutronSource
      if (getParam<bool>("account_delayed"))
        addDelayedNeutronSource(op, var_name);
    }

    if (_current_task == "add_bc")
    {
      if (isParamValid("vacuum_boundaries"))
      {
        // Set up vacuum boundary conditions
        InputParameters params = _factory.getValidParams("VacuumConcBC");
        params.set<std::vector<BoundaryName>>("boundary") =
            getParam<std::vector<BoundaryName>>("vacuum_boundaries");
        params.set<NonlinearVariableName>("variable") = var_name;
        if (isParamValid("use_exp_form"))
          params.set<bool>("use_exp_form") = getParam<bool>("use_exp_form");
        params.set<MooseEnum>("vacuum_bc_type") = getParam<MooseEnum>("vacuum_bc_type");
        std::string bc_name = "VacuumConcBC_" + var_name;
        _problem->addBoundaryCondition("VacuumConcBC", bc_name, params);
      }
    }

    if (_current_task == "add_ic" && !getParam<bool>("init_nts_from_file"))
    {
      if (getParam<bool>("jac_test") && isParamValid("nt_ic_function"))
        mooseError("jac_test creates RandomICs. So are you sure you want to pass an initial "
                   "condition function?");
      if (getParam<bool>("jac_test"))
      {
        InputParameters params = _factory.getValidParams("RandomIC");
        params.set<VariableName>("variable") = var_name;
        if (isParamValid("block"))
          params.set<std::vector<SubdomainName>>("block") =
              getParam<std::vector<SubdomainName>>("block");
        params.set<Real>("min") = 0;
        params.set<Real>("max") = 1;

        std::string ic_name = "RandomIC_" + var_name;
        _problem->addInitialCondition("RandomIC", ic_name, params);
      }
      else if (isParamValid("nt_ic_function"))
      {
        InputParameters params = _factory.getValidParams("FunctionIC");
        params.set<VariableName>("variable") = var_name;
        if (isParamValid("block"))
          params.set<std::vector<SubdomainName>>("block") =
              getParam<std::vector<SubdomainName>>("block");
        params.set<FunctionName>("function") = getParam<FunctionName>("nt_ic_function");

        std::string ic_name = "FunctionIC_" + var_name;
        _problem->addInitialCondition("FunctionIC", ic_name, params);
      }
      else
      {
        InputParameters params = _factory.getValidParams("ConstantIC");
        params.set<VariableName>("variable") = var_name;
        if (isParamValid("block"))
          params.set<std::vector<SubdomainName>>("block") =
              getParam<std::vector<SubdomainName>>("block");
        if (getParam<bool>("use_exp_form"))
          params.set<Real>("value") = 0;
        else
          params.set<Real>("value") = 1;

        std::string ic_name = "ConstantIC_" + var_name;
        _problem->addInitialCondition("ConstantIC", ic_name, params);
      }
    }

    if (getParam<bool>("use_exp_form"))
    {
      std::string aux_var_name = var_name + "_lin";
      // Set up nodal aux variables
      if (_current_task == "add_aux_variable")
      {
        std::set<SubdomainID> blocks = getSubdomainIDs();
        FEType fe_type(FIRST, LAGRANGE);
        if (blocks.empty())
          _problem->addAuxVariable(aux_var_name, fe_type);
        else
          _problem->addAuxVariable(aux_var_name, fe_type, &blocks);
      }
      // Set up aux kernels
      if (_current_task == "add_aux_kernel")
      {
        InputParameters params = _factory.getValidParams("Density");
        params.set<AuxVariableName>("variable") = aux_var_name;
        params.set<std::vector<VariableName>>("density_log") = {var_name};
        if (isParamValid("block"))
          params.set<std::vector<SubdomainName>>("block") =
              getParam<std::vector<SubdomainName>>("block");

        std::string aux_kernel_name = "Density_" + aux_var_name;
        _problem->addAuxKernel("Density", aux_kernel_name, params);
      }
    }
  }

  if (getParam<bool>("create_temperature_var"))
  {
    std::string temp_var = "temp";
    // See whether we want to use an old solution
    if (getParam<bool>("init_temperature_from_file"))
    {
      if (_current_task == "check_copy_nodal_vars")
        _app.setExodusFileRestart(true);

      if (_current_task == "copy_nodal_vars")
      {
        SystemBase * system;
        system = &_problem->getNonlinearSystemBase(/*nl_sys_num=*/0);
        system->addVariableToCopy(temp_var, temp_var, "LATEST");
      }
    }

    if (_current_task == "add_variable")
    {
      FEType fe_type(getParam<bool>("dg_for_temperature") ? FIRST : FIRST,
                     getParam<bool>("dg_for_temperature") ? L2_LAGRANGE : LAGRANGE);
      const auto variable_type = AddVariableAction::variableType(fe_type);
      auto params = _factory.getValidParams(variable_type);

      params.set<MooseEnum>("order") =
          libMesh::Utility::enum_to_string(fe_type.order.operator Order());
      params.set<MooseEnum>("family") = libMesh::Utility::enum_to_string(fe_type.family);
      params.set<std::vector<Real>>("scaling") = {
          isParamValid("temp_scaling") ? getParam<Real>("temp_scaling") : 1};
      _problem->addVariable(variable_type, temp_var, params);
    }
  }
}

void
NtAction::addNtKernel(const unsigned & op,
                      const std::string & var_name,
                      const std::string & kernel_type,
                      const std::vector<VariableName> & all_var_names)
{
  InputParameters params = _factory.getValidParams(kernel_type);
  params.set<NonlinearVariableName>("variable") = var_name;
  params.set<unsigned int>("group_number") = op;
  if (isParamValid("block"))
    params.set<std::vector<SubdomainName>>("block") =
        getParam<std::vector<SubdomainName>>("block");
  if (isParamValid("use_exp_form"))
    params.set<bool>("use_exp_form") = getParam<bool>("use_exp_form");
  std::vector<std::string> include = {"temperature"};
  params.applySpecificParameters(parameters(), include);
  if (kernel_type == "InScatter")
  {
    params.set<unsigned int>("num_groups") = _num_groups;
    params.set<bool>("sss2_input") = getParam<bool>("sss2_input");
    params.set<std::vector<VariableName>>("group_fluxes") = all_var_names;
  }
  std::string kernel_name = kernel_type + "_" + var_name;
  _problem->addKernel(kernel_type, kernel_name, params);
}

void
NtAction::addCoupledFissionKernel(const unsigned & op,
                                  const std::string & var_name,
                                  const std::vector<VariableName> & all_var_names)
{
  InputParameters params = _factory.getValidParams("CoupledFissionKernel");
  params.set<NonlinearVariableName>("variable") = var_name;
  params.set<unsigned int>("group_number") = op;
  if (isParamValid("block"))
    params.set<std::vector<SubdomainName>>("block") =
        getParam<std::vector<SubdomainName>>("block");
  if (isParamValid("use_exp_form"))
    params.set<bool>("use_exp_form") = getParam<bool>("use_exp_form");
  std::vector<std::string> include = {"temperature"};
  params.applySpecificParameters(parameters(), include);
  params.set<unsigned int>("num_groups") = _num_groups;
  params.set<std::vector<VariableName>>("group_fluxes") = all_var_names;
  params.set<bool>("account_delayed") = getParam<bool>("account_delayed");
  params.set<Real>("eigenvalue_scaling") = getParam<Real>("eigenvalue_scaling");
  if (getParam<bool>("eigen"))
    params.set<std::vector<TagName>>("extra_vector_tags") = {"eigen"};
  std::string kernel_name = "CoupledFissionKernel_" + var_name;
  _problem->addKernel("CoupledFissionKernel", kernel_name, params);
}

void
NtAction::addDelayedNeutronSource(const unsigned & op, const std::string & var_name)
{
  InputParameters params = _factory.getValidParams("DelayedNeutronSource");
  params.set<NonlinearVariableName>("variable") = var_name;
  params.set<unsigned int>("group_number") = op;
  if (isParamValid("pre_blocks"))
    params.set<std::vector<SubdomainName>>("block") =
        getParam<std::vector<SubdomainName>>("pre_blocks");
  if (isParamValid("use_exp_form"))
    params.set<bool>("use_exp_form") = getParam<bool>("use_exp_form");
  std::vector<std::string> include = {"temperature", "pre_concs"};
  params.applySpecificParameters(parameters(), include);
  params.set<unsigned int>("num_precursor_groups") = _num_precursor_groups;
  std::string kernel_name = "DelayedNeutronSource_" + var_name;
  _problem->addKernel("DelayedNeutronSource", kernel_name, params);
}
