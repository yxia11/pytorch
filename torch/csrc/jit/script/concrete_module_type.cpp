#include <torch/csrc/jit/script/concrete_module_type.h>

namespace torch {
namespace jit {
namespace script {
namespace {
ClassTypePtr createTypeFromData(const ConcreteModuleTypeData& data) {
  auto cu = get_python_cu();
  py::object pyQualName = py::module::import("torch._jit_internal")
                              .attr("_qualified_name")(data.pyClass_);

  auto className = c10::QualifiedName(py::cast<std::string>(pyQualName));
  if (className.prefix().empty()) {
    className = c10::QualifiedName("__torch__", className.name());
  }
  if (cu->get_class(className) != nullptr) {
    className = cu->mangle(className);
  }
  auto cls = ClassType::create(std::move(className), cu, /*is_module=*/true);
  cu->register_type(cls);

  // populate type with info from the concrete type information
  for (const auto& pr : data.attributes_) {
    const auto& name = pr.first;
    const auto& type = pr.second.type_;
    const auto& isParameter = pr.second.isParam_;

    cls->addAttribute(name, type, isParameter);
  }

  for (const auto& moduleInfo : data.modules_) {
    cls->addAttribute(
        moduleInfo.name_, moduleInfo.getJitType(), /*is_parameter=*/false);
  }

  return cls;
}
} // namespace

ConcreteModuleType::ConcreteModuleType(ConcreteModuleTypeData data)
    : data_(std::move(data)) {
  jitType_ = createTypeFromData(data_);
}

TypePtr ConcreteModuleTypeData::ModuleInfo::getJitType() const {
  return meta_ == nullptr ? type_ : meta_->getJitType();
}

bool operator==(
    const ConcreteModuleTypeData::ModuleInfo& lhs,
    const ConcreteModuleTypeData::ModuleInfo& rhs) {
  if (lhs.meta_ != nullptr && rhs.meta_ != nullptr) {
    return lhs.meta_->equals(*rhs.meta_);
  } else if (lhs.type_ != nullptr && rhs.type_ != nullptr) {
    return *(lhs.type_) == *(rhs.type_);
  } else {
    return false;
  }
}

bool RawConcreteModuleType::equals(
    const RawConcreteModuleType& other) const {
  return data_ == other.data_;
}

bool RawConcreteModuleType::equals(
    const ConcreteModuleType& other) const {
  return data_ == other.data_;
}

ClassTypePtr ConcreteModuleType::getJitType() const {
  return jitType_;
}

py::object ConcreteModuleType::getPyClass() const {
  return data_.pyClass_;
}

c10::optional<std::vector<std::string>> ConcreteModuleType::findOverloads(
    const std::string& name) const {
  const auto it = data_.overloads_.find(name);
  if (it != data_.overloads_.end()) {
    return it->second;
  }
  return c10::nullopt;
}

c10::optional<Function*> ConcreteModuleType::findFunctionAttribute(
    const std::string& name) const {
  const auto it = data_.functionAttributes_.find(name);
  if (it != data_.functionAttributes_.end()) {
    return it->second.function_->function();
  }
  return c10::nullopt;
}

c10::optional<std::string> ConcreteModuleType::findFailedAttribute(
    const std::string& name) const {
  const auto it = data_.failedAttributes_.find(name);
  if (it != data_.failedAttributes_.end()) {
    return it->second;
  }
  return c10::nullopt;
}

std::shared_ptr<ConcreteModuleType> ConcreteModuleType::
    findSubmoduleConcreteType(const std::string& name) const {
  const auto it = std::find_if(
      data_.modules_.cbegin(),
      data_.modules_.cend(),
      [&](const ConcreteModuleTypeData::ModuleInfo& info) {
        return info.name_ == name;
      });
  if (it == data_.modules_.end()) {
    return nullptr;
  }
  return it->meta_;
}

void RawConcreteModuleType::setIterableModuleKind(IterableModuleKind kind) {
  data_.iterableModuleKind_ = kind;
}

IterableModuleKind ConcreteModuleType::getIterableModuleKind() const {
  return data_.iterableModuleKind_;
}

void RawConcreteModuleType::setPoisoned() {
  data_.isPoisoned_ = true;
}

void RawConcreteModuleType::addConstant(std::string name, py::object value) {
  data_.constants_.emplace(std::move(name), std::move(value));
}

void RawConcreteModuleType::addAttribute(
    std::string name,
    TypePtr type,
    bool isParameter) {
  TORCH_INTERNAL_ASSERT(type);
  // Function attributes should be handled separately
  TORCH_INTERNAL_ASSERT(type->cast<FunctionType>() == nullptr);
  data_.attributes_.emplace(
      std::move(name),
      ConcreteModuleTypeData::Attribute(unshapedType(type), isParameter));
}

void RawConcreteModuleType::addFunctionAttribute(
    std::string name,
    const TypePtr& type,
    py::object pyFunction) {
  TORCH_INTERNAL_ASSERT(type);
  data_.functionAttributes_.emplace(
      std::move(name),
      ConcreteModuleTypeData::FunctionAttribute{type->expect<FunctionType>(),
                                                std::move(pyFunction)});
}

void RawConcreteModuleType::addModule(
    std::string name,
    std::shared_ptr<ConcreteModuleType> meta) {
  data_.modules_.emplace_back(
      ConcreteModuleTypeData::ModuleInfo{std::move(name), std::move(meta)});
}

void RawConcreteModuleType::addModuleInterface(
    std::string name,
    const TypePtr& type) {
  TORCH_INTERNAL_ASSERT(type->cast<InterfaceType>() && type->is_module());
  data_.modules_.emplace_back(
      ConcreteModuleTypeData::ModuleInfo{std::move(name), type});
}

void RawConcreteModuleType::addOverload(
    std::string methodName,
    std::vector<std::string> overloadedMethodNames) {
  data_.overloads_.emplace(std::move(methodName), std::move(overloadedMethodNames));
}

void RawConcreteModuleType::addFailedAttribute(
    std::string name,
    std::string failureReason) {
  data_.failedAttributes_.emplace(std::move(name), std::move(failureReason));
}

c10::optional<py::object> ConcreteModuleType::findConstant(
    const std::string& name) const {
  auto it = data_.constants_.find(name);
  if (it != data_.constants_.end()) {
    return it->second.v_;
  }
  return c10::nullopt;
}

void ConcreteModuleType::dump() const {
  std::cout << "ConcreteModuleType for: " << py::getattr(data_.pyClass_, "__name__") << "\n";
  std::cout << "Constants: \n";
  for (const auto& pr : data_.constants_) {
    std::cout << "\t" << pr.first << ": " << pr.second.v_ << "\n";
  }
  std::cout << "\nAttributes: \n";
  for (const auto& pr : data_.attributes_) {
    std::cout << "\t" << pr.first << ": " << pr.second.type_->python_str()
              << "\n";
  }
  std::cout << "\nSubmodules: \n";
  for (const auto& info : data_.modules_) {
    std::cout << "\t" << info.name_ << ": "
          << info.getJitType()->python_str() << "\n";
  }
  std::cout << "\nOverloads: \n";
  for (const auto& pr : data_.overloads_) {
    std::cout << "\t" << pr.first << ": " << pr.second << "\n";
  }
  std::string isPoisoned = data_.isPoisoned_ ? "true" : "false";
  std::cout << "isPoisoned: " << isPoisoned << "\n";
  if (jitType_) {
    std::cout << "jit type: " << jitType_->python_str() << "\n";
  }
}

std::unordered_map<std::string, py::object> ConcreteModuleType::getConstantsPy()
    const {
  // Convert to a more pybind-friendly representation, so we don't
  // need to bind ConcreteModuleType::Constant as well.
  std::unordered_map<std::string, py::object> ret;
  for (const auto& pr : data_.constants_) {
    ret.emplace(pr.first, pr.second.v_);
  }
  return ret;
}

std::unordered_map<std::string, std::pair<TypePtr, bool>> ConcreteModuleType::
    getAttributesPy() const {
  // Convert to a more pybind-friendly representation, so we don't
  // need to bind ConcreteModuleType::Attribute as well.
  std::unordered_map<std::string, std::pair<TypePtr, bool>> ret;
  for (auto& pr : data_.attributes_) {
    ret.emplace(
        pr.first,
        std::pair<TypePtr, bool>(pr.second.type_, pr.second.isParam_));
  }
  return ret;
}

std::vector<std::pair<std::string, TypePtr>> ConcreteModuleType::getModulesPy()
    const {
  std::vector<std::pair<std::string, TypePtr>> ret;

  for (const auto& info : data_.modules_) {
    ret.emplace_back(std::make_pair(info.name_, info.getJitType()));
  }
  return ret;
}

} // namespace script
} // namespace jit
} // namespace torch
