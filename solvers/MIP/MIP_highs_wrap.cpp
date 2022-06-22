/* -*- mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */

/*
 *  Main authors:
 *     Jip J. Dekker <jip.dekker@monash.edu>
 */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <minizinc/exception.hh>
#include <minizinc/solvers/MIP/MIP_highs_wrap.hh>
#include <minizinc/solvers/MIP/MIP_wrap.hh>

#include <string>

using namespace MiniZinc;

std::string MIPHiGHSWrapper::getVersion(FactoryOptions& factoryOpt,
                                        MiniZinc::SolverInstanceBase::Options* /*opt*/) {
  std::ostringstream ss;
  ss << HIGHS_VERSION_MAJOR << "." << HIGHS_VERSION_MINOR << "." << HIGHS_VERSION_PATCH;
  return ss.str();
}
std::string MIPHiGHSWrapper::getDescription(FactoryOptions& factoryOpt,
                                            MiniZinc::SolverInstanceBase::Options* /*opt*/) {
  std::ostringstream ss;
  ss << "MIP wrapper for HiGHS " << HIGHS_VERSION_MAJOR << "." << HIGHS_VERSION_MINOR << "."
     << HIGHS_VERSION_PATCH << "\n  Compiled  " << HIGHS_COMPILATION_DATE << ", git hash "
     << HIGHS_GITHASH << "\n";
  return ss.str();
}

void MIPHiGHSWrapper::Options::printHelp(std::ostream& os) {
  os << "HiGHS MIP wrapper options:"
     << std::endl
     // << "--backend-flags \"args\"" << std::endl
     // << "    command-line args passed to callCbc, e.g., \"-cuts off -preprocess off -passc 1\"."
     // << std::endl
     << "  --writeModel <file>" << std::endl
     << "    write model to <file> (.mps)" << std::endl
     << "  -i" << std::endl
     << "    print intermediate solutions for optimization problems" << std::endl
     << "  -p <N>, --parallel <N>\n    use N threads, default: 1." << std::endl
     << "  --solver-time-limit <N>\n    stop search after N milliseconds" << std::endl
     << "  --absGap <n>\n    absolute gap |primal-dual| to stop" << std::endl
     << "  --relGap <n>\n    relative gap |primal-dual|/<solver-dep> to stop. Default 1e-8, set <0 "
        "to use backend's default"
     << std::endl
     << "  --intTol <n>\n    integrality tolerance for a variable. Default 1e-8" << std::endl;
}

bool MIPHiGHSWrapper::Options::processOption(int& i, std::vector<std::string>& argv,
                                             const std::string& workingDir) {
  MiniZinc::CLOParser cop(i, argv);
  std::string buffer;
  if (cop.get("-i")) {
    flagIntermediate = true;
  } else if (string(argv[i]) == "-f") {  // NOLINT: Allow repeated empty if
  } else if (cop.get("--writeModel", &buffer)) {
    sExportModel = MiniZinc::FileUtils::file_path(buffer, workingDir);
  } else if (cop.get("-p --parallel", &nThreads)) {        // NOLINT: Allow repeated empty if
  } else if (cop.get("--solver-time-limit", &nTimeout)) {  // NOLINT: Allow repeated empty if
  } else if (cop.get("--absGap", &absGap)) {               // NOLINT: Allow repeated empty if
  } else if (cop.get("--relGap", &relGap)) {               // NOLINT: Allow repeated empty if
  } else if (cop.get("--intTol", &intTol)) {               // NOLINT: Allow repeated empty if
  } else {
    return false;
  }
  return true;
}

std::vector<SolverConfig::ExtraFlag> MIPHiGHSWrapper::getExtraFlags(FactoryOptions& factoryOpt) {
  HighsOptions options;

  std::vector<MiniZinc::SolverConfig::ExtraFlag> res;
  res.reserve(options.records.size());
  for (const auto* record : options.records) {
    MiniZinc::SolverConfig::ExtraFlag::FlagType t;
    std::string default_val;
    std::vector<std::string> range;
    if (const auto* br = dynamic_cast<const OptionRecordBool*>(record)) {
      t = SolverConfig::ExtraFlag::FlagType::T_BOOL;
      default_val = br->default_value ? "true" : "false";
    } else if (const auto* ir = dynamic_cast<const OptionRecordInt*>(record)) {
      t = SolverConfig::ExtraFlag::FlagType::T_INT;
      default_val = std::to_string(ir->default_value);
      range = {std::to_string(ir->lower_bound), std::to_string(ir->upper_bound)};
    } else if (const auto* dr = dynamic_cast<const OptionRecordDouble*>(record)) {
      t = SolverConfig::ExtraFlag::FlagType::T_FLOAT;
      default_val = std::to_string(dr->default_value);
      range = {std::to_string(dr->lower_bound), std::to_string(dr->upper_bound)};
    } else if (const auto* sr = dynamic_cast<const OptionRecordString*>(record)) {
      t = SolverConfig::ExtraFlag::FlagType::T_STRING;
      default_val = sr->default_value;
    } else {
      assert(false);
    }
    res.emplace_back("--highs-" + record->name, record->description, t, std::move(range),
                     std::move(default_val));
  }
  return res;
}

/// Add new variables to the solver
void MIPHiGHSWrapper::doAddVars(size_t n, double* obj, double* lb, double* ub, VarType* vt,
                                std::string* names) {
  HighsInt cur = _highs.getNumCol();
  checkHiGHSReturn(_highs.addCols(static_cast<const HighsInt>(n), obj, lb, ub, 0, nullptr,
                                  nullptr, nullptr),
                   "failed to add new variables");
  assert(cur + n == _highs.getNumCol());
  std::vector<HighsVarType> types;
  types.reserve(n);
  for (int i = 0; i < n; ++i) {
    switch (vt[i]) {
      case VarType::REAL: {
        types.push_back(HighsVarType::kContinuous);
        break;
      }
      case VarType::BINARY:
      case VarType::INT: {
        types.push_back(HighsVarType::kInteger);
        break;
      }
    }
  }
  checkHiGHSReturn(_highs.changeColsIntegrality(cur, _highs.getNumCol() - 1, types.data()),
                   "unable to set integrality constraint");
}

/// Add a linear constraint
void MIPHiGHSWrapper::addRow(int nnz, int* rmatind, double* rmatval, LinConType sense, double rhs,
                             int mask, const std::string& rowName) {
  /// Convert var types:
  double rlb = rhs;
  double rub = rhs;
  switch (sense) {
    case LQ:
      rlb = -_highs.getInfinity();
      break;
    case EQ:
      break;
    case GQ:
      rub = _highs.getInfinity();
      break;
    default:
      throw MiniZinc::InternalError("MIPWrapper: unknown constraint type");
  }
  HighsStatus res = _highs.addRow(rlb, rub, nnz, rmatind, rmatval);
  checkHiGHSReturn(res, "HiGHS Error: Unable to add linear constraint");
}

void MIPHiGHSWrapper::solve() {
  // Set solver parameters
  if (_options->nThreads > 0) {
    checkHiGHSReturn(_highs.setOptionValue("threads", _options->nThreads),
                     "unable to set number of threads");
    checkHiGHSReturn(_highs.setOptionValue(kParallelString, "on"),
                     "unable to enable parallel mode");
  } else {
    checkHiGHSReturn(_highs.setOptionValue(kParallelString, "off"),
                     "unable to disable parallel mode");
  }
  if (_options->nTimeout > 0) {
    checkHiGHSReturn(
        _highs.setOptionValue(kTimeLimitString, static_cast<double>(_options->nTimeout) / 1000.0),
        "unable to time limit");
  }
  if (_options->absGap >= 0.0) {
    checkHiGHSReturn(_highs.setOptionValue("mip_abs_gap", _options->absGap),
                     "unable to set absolute gap");
  }
  if (_options->relGap >= 0.0) {
    checkHiGHSReturn(_highs.setOptionValue("mip_rel_gap", _options->relGap),
                     "unable to set relative gap");
  }
  if (_options->intTol >= 0.0) {
    checkHiGHSReturn(_highs.setOptionValue("mip_feasibility_tolerance", _options->intTol),
                     "unable to set integer tolerance");
  }
  if (!_options->sExportModel.empty()) {
    checkHiGHSReturn(_highs.writeModel(_options->sExportModel), "Unable to write model to file");
  }
  if (fVerbose) {
    checkHiGHSReturn(_highs.setOptionValue("log_dev_level", kHighsLogDevLevelDetailed),
                     "Unable to set verbose flag");
  } else {
    checkHiGHSReturn(_highs.setOptionValue("log_dev_level", kHighsLogDevLevelNone),
                     "Unable to set logging to silent");
  }

  for (const auto& it : _options->extraParams) {
    std::string name = it.first.substr(8);
    HighsOptionType type;
    checkHiGHSReturn(_highs.getOptionType(name, type),
                     "Unable to find type of option `" + name + "'");
    switch (type) {
      case HighsOptionType::kBool: {
        assert(it.second == "true" || it.second == "false");
        checkHiGHSReturn(_highs.setOptionValue(name, it.second == "true"),
                         "unable to set HiGHS option `" + name + "'");
        break;
      }
      case HighsOptionType::kInt: {
        checkHiGHSReturn(_highs.setOptionValue(name, stoi(it.second)),
                         "unable to set HiGHS option `" + name + "'");
        break;
      }
      case HighsOptionType::kDouble: {
        checkHiGHSReturn(_highs.setOptionValue(name, stod(it.second)),
                         "unable to set HiGHS option `" + name + "'");
        break;
      }
      case HighsOptionType::kString: {
        checkHiGHSReturn(_highs.setOptionValue(name, it.second),
                         "unable to set HiGHS option `" + name + "'");
        break;
      }
      default:
        throw InternalError("Unknown HiGHS Option type");
    }
  }

  // Actually solve the current model
  checkHiGHSReturn(_highs.run(), "unable to solve model");

  // Process Results
  output.status = convertStatus(_highs.getModelStatus());
  output.statusName = _highs.modelStatusToString(_highs.getModelStatus());
  output.objVal = _highs.getObjectiveValue();
  output.bestBound = _highs.getInfo().mip_dual_bound;
  output.nNodes = static_cast<int>(_highs.getInfo().mip_node_count);
  if (getStatus() == MIPWrapper::SAT || getStatus() == MIPWrapper::OPT) {
    output.x = _highs.getSolution().col_value.data();
    if (cbui.solcbfn != nullptr && (!_options->flagIntermediate || !cbui.printed)) {
      cbui.solcbfn(output, cbui.psi);
    }
  }
}

MIPWrapper::Status MIPHiGHSWrapper::convertStatus(const HighsModelStatus& model_status) const {
  switch (model_status) {
    case HighsModelStatus::kNotset:
    case HighsModelStatus::kUnknown: {
      return MIPWrapper::UNKNOWN;
    }
    case HighsModelStatus::kLoadError:
    case HighsModelStatus::kModelError:
    case HighsModelStatus::kPresolveError:
    case HighsModelStatus::kSolveError:
    case HighsModelStatus::kPostsolveError:
    case HighsModelStatus::kModelEmpty: {
      return MIPWrapper::ERROR_STATUS;
    }
    case HighsModelStatus::kOptimal: {
      return MIPWrapper::OPT;
    }
    case HighsModelStatus::kInfeasible: {
      return MIPWrapper::UNSAT;
    }
    case HighsModelStatus::kUnbounded: {
      return MIPWrapper::UNBND;
    }
    case HighsModelStatus::kUnboundedOrInfeasible: {
      return MIPWrapper::UNSATorUNBND;
    }
    case HighsModelStatus::kObjectiveBound:
    case HighsModelStatus::kObjectiveTarget: {
      assert(false);
      return MIPWrapper::ERROR_STATUS;
    }
    case HighsModelStatus::kTimeLimit:
    case HighsModelStatus::kIterationLimit: {
      // TODO: is this a good check whether a solution has been found?
      if (_highs.getSolution().value_valid) {
        return MIPWrapper::SAT;
      }
      return MIPWrapper::UNKNOWN;
    }
    default: {
      throw InternalError("Unknown HiGHS status");
    }
  }
}