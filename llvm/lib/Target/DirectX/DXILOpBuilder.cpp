//===- DXILOpBuilder.cpp - Helper class for build DIXLOp functions --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file This file contains class to help build DXIL op functions.
//===----------------------------------------------------------------------===//

#include "DXILOpBuilder.h"
#include "DXILConstants.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/DXILABI.h"
#include "llvm/Support/ErrorHandling.h"
#include <optional>

using namespace llvm;
using namespace llvm::dxil;

constexpr StringLiteral DXILOpNamePrefix = "dx.op.";

namespace {
enum OverloadKind : uint16_t {
  UNDEFINED = 0,
  VOID = 1,
  HALF = 1 << 1,
  FLOAT = 1 << 2,
  DOUBLE = 1 << 3,
  I1 = 1 << 4,
  I8 = 1 << 5,
  I16 = 1 << 6,
  I32 = 1 << 7,
  I64 = 1 << 8,
  UserDefineType = 1 << 9,
  ObjectType = 1 << 10,
};
struct Version {
  unsigned Major = 0;
  unsigned Minor = 0;
};

struct OpOverload {
  Version DXILVersion;
  uint16_t ValidTys;
};
} // namespace

struct OpStage {
  Version DXILVersion;
  uint32_t ValidStages;
};

static const char *getOverloadTypeName(OverloadKind Kind) {
  switch (Kind) {
  case OverloadKind::HALF:
    return "f16";
  case OverloadKind::FLOAT:
    return "f32";
  case OverloadKind::DOUBLE:
    return "f64";
  case OverloadKind::I1:
    return "i1";
  case OverloadKind::I8:
    return "i8";
  case OverloadKind::I16:
    return "i16";
  case OverloadKind::I32:
    return "i32";
  case OverloadKind::I64:
    return "i64";
  case OverloadKind::VOID:
  case OverloadKind::UNDEFINED:
    return "void";
  case OverloadKind::ObjectType:
  case OverloadKind::UserDefineType:
    break;
  }
  llvm_unreachable("invalid overload type for name");
}

static OverloadKind getOverloadKind(Type *Ty) {
  if (!Ty)
    return OverloadKind::VOID;

  Type::TypeID T = Ty->getTypeID();
  switch (T) {
  case Type::VoidTyID:
    return OverloadKind::VOID;
  case Type::HalfTyID:
    return OverloadKind::HALF;
  case Type::FloatTyID:
    return OverloadKind::FLOAT;
  case Type::DoubleTyID:
    return OverloadKind::DOUBLE;
  case Type::IntegerTyID: {
    IntegerType *ITy = cast<IntegerType>(Ty);
    unsigned Bits = ITy->getBitWidth();
    switch (Bits) {
    case 1:
      return OverloadKind::I1;
    case 8:
      return OverloadKind::I8;
    case 16:
      return OverloadKind::I16;
    case 32:
      return OverloadKind::I32;
    case 64:
      return OverloadKind::I64;
    default:
      llvm_unreachable("invalid overload type");
      return OverloadKind::VOID;
    }
  }
  case Type::PointerTyID:
    return OverloadKind::UserDefineType;
  case Type::StructTyID: {
    // TODO: This is a hack. As described in DXILEmitter.cpp, we need to rework
    // how we're handling overloads and remove the `OverloadKind` proxy enum.
    StructType *ST = cast<StructType>(Ty);
    return getOverloadKind(ST->getElementType(0));
  }
  default:
    return OverloadKind::UNDEFINED;
  }
}

static std::string getTypeName(OverloadKind Kind, Type *Ty) {
  if (Kind < OverloadKind::UserDefineType) {
    return getOverloadTypeName(Kind);
  } else if (Kind == OverloadKind::UserDefineType) {
    StructType *ST = cast<StructType>(Ty);
    return ST->getStructName().str();
  } else if (Kind == OverloadKind::ObjectType) {
    StructType *ST = cast<StructType>(Ty);
    return ST->getStructName().str();
  } else {
    std::string Str;
    raw_string_ostream OS(Str);
    Ty->print(OS);
    return OS.str();
  }
}

// Static properties.
struct OpCodeProperty {
  dxil::OpCode OpCode;
  // Offset in DXILOpCodeNameTable.
  unsigned OpCodeNameOffset;
  dxil::OpCodeClass OpCodeClass;
  // Offset in DXILOpCodeClassNameTable.
  unsigned OpCodeClassNameOffset;
  llvm::SmallVector<OpOverload> Overloads;
  llvm::SmallVector<OpStage> Stages;
  int OverloadParamIndex; // parameter index which control the overload.
                          // When < 0, should be only 1 overload type.
};

// Include getOpCodeClassName getOpCodeProperty, getOpCodeName and
// getOpCodeParameterKind which generated by tableGen.
#define DXIL_OP_OPERATION_TABLE
#include "DXILOperation.inc"
#undef DXIL_OP_OPERATION_TABLE

static std::string constructOverloadName(OverloadKind Kind, Type *Ty,
                                         const OpCodeProperty &Prop) {
  if (Kind == OverloadKind::VOID) {
    return (Twine(DXILOpNamePrefix) + getOpCodeClassName(Prop)).str();
  }
  return (Twine(DXILOpNamePrefix) + getOpCodeClassName(Prop) + "." +
          getTypeName(Kind, Ty))
      .str();
}

static std::string constructOverloadTypeName(OverloadKind Kind,
                                             StringRef TypeName) {
  if (Kind == OverloadKind::VOID)
    return TypeName.str();

  assert(Kind < OverloadKind::UserDefineType && "invalid overload kind");
  return (Twine(TypeName) + getOverloadTypeName(Kind)).str();
}

static StructType *getOrCreateStructType(StringRef Name,
                                         ArrayRef<Type *> EltTys,
                                         LLVMContext &Ctx) {
  StructType *ST = StructType::getTypeByName(Ctx, Name);
  if (ST)
    return ST;

  return StructType::create(Ctx, EltTys, Name);
}

static StructType *getResRetType(Type *ElementTy) {
  LLVMContext &Ctx = ElementTy->getContext();
  OverloadKind Kind = getOverloadKind(ElementTy);
  std::string TypeName = constructOverloadTypeName(Kind, "dx.types.ResRet.");
  Type *FieldTypes[5] = {ElementTy, ElementTy, ElementTy, ElementTy,
                         Type::getInt32Ty(Ctx)};
  return getOrCreateStructType(TypeName, FieldTypes, Ctx);
}

static StructType *getCBufRetType(Type *ElementTy) {
  LLVMContext &Ctx = ElementTy->getContext();
  OverloadKind Kind = getOverloadKind(ElementTy);
  std::string TypeName = constructOverloadTypeName(Kind, "dx.types.CBufRet.");

  // 64-bit types only have two elements
  if (ElementTy->isDoubleTy() || ElementTy->isIntegerTy(64))
    return getOrCreateStructType(TypeName, {ElementTy, ElementTy}, Ctx);

  // 16-bit types pack 8 elements and have .8 in their name to differentiate
  // from min-precision types.
  if (ElementTy->isHalfTy() || ElementTy->isIntegerTy(16)) {
    TypeName += ".8";
    return getOrCreateStructType(TypeName,
                                 {ElementTy, ElementTy, ElementTy, ElementTy,
                                  ElementTy, ElementTy, ElementTy, ElementTy},
                                 Ctx);
  }

  return getOrCreateStructType(
      TypeName, {ElementTy, ElementTy, ElementTy, ElementTy}, Ctx);
}

static StructType *getHandleType(LLVMContext &Ctx) {
  return getOrCreateStructType("dx.types.Handle", PointerType::getUnqual(Ctx),
                               Ctx);
}

static StructType *getResBindType(LLVMContext &Context) {
  if (auto *ST = StructType::getTypeByName(Context, "dx.types.ResBind"))
    return ST;
  Type *Int32Ty = Type::getInt32Ty(Context);
  Type *Int8Ty = Type::getInt8Ty(Context);
  return StructType::create({Int32Ty, Int32Ty, Int32Ty, Int8Ty},
                            "dx.types.ResBind");
}

static StructType *getResPropsType(LLVMContext &Context) {
  if (auto *ST =
          StructType::getTypeByName(Context, "dx.types.ResourceProperties"))
    return ST;
  Type *Int32Ty = Type::getInt32Ty(Context);
  return StructType::create({Int32Ty, Int32Ty}, "dx.types.ResourceProperties");
}

static StructType *getSplitDoubleType(LLVMContext &Context) {
  if (auto *ST = StructType::getTypeByName(Context, "dx.types.splitdouble"))
    return ST;
  Type *Int32Ty = Type::getInt32Ty(Context);
  return StructType::create({Int32Ty, Int32Ty}, "dx.types.splitdouble");
}

static StructType *getBinaryWithCarryType(LLVMContext &Context) {
  if (auto *ST = StructType::getTypeByName(Context, "dx.types.i32c"))
    return ST;
  Type *Int32Ty = Type::getInt32Ty(Context);
  Type *Int1Ty = Type::getInt1Ty(Context);
  return StructType::create({Int32Ty, Int1Ty}, "dx.types.i32c");
}

static Type *getTypeFromOpParamType(OpParamType Kind, LLVMContext &Ctx,
                                    Type *OverloadTy) {
  switch (Kind) {
  case OpParamType::VoidTy:
    return Type::getVoidTy(Ctx);
  case OpParamType::HalfTy:
    return Type::getHalfTy(Ctx);
  case OpParamType::FloatTy:
    return Type::getFloatTy(Ctx);
  case OpParamType::DoubleTy:
    return Type::getDoubleTy(Ctx);
  case OpParamType::Int1Ty:
    return Type::getInt1Ty(Ctx);
  case OpParamType::Int8Ty:
    return Type::getInt8Ty(Ctx);
  case OpParamType::Int16Ty:
    return Type::getInt16Ty(Ctx);
  case OpParamType::Int32Ty:
    return Type::getInt32Ty(Ctx);
  case OpParamType::Int64Ty:
    return Type::getInt64Ty(Ctx);
  case OpParamType::OverloadTy:
    return OverloadTy;
  case OpParamType::ResRetHalfTy:
    return getResRetType(Type::getHalfTy(Ctx));
  case OpParamType::ResRetFloatTy:
    return getResRetType(Type::getFloatTy(Ctx));
  case OpParamType::ResRetDoubleTy:
    return getResRetType(Type::getDoubleTy(Ctx));
  case OpParamType::ResRetInt16Ty:
    return getResRetType(Type::getInt16Ty(Ctx));
  case OpParamType::ResRetInt32Ty:
    return getResRetType(Type::getInt32Ty(Ctx));
  case OpParamType::ResRetInt64Ty:
    return getResRetType(Type::getInt64Ty(Ctx));
  case OpParamType::CBufRetHalfTy:
    return getCBufRetType(Type::getHalfTy(Ctx));
  case OpParamType::CBufRetFloatTy:
    return getCBufRetType(Type::getFloatTy(Ctx));
  case OpParamType::CBufRetDoubleTy:
    return getCBufRetType(Type::getDoubleTy(Ctx));
  case OpParamType::CBufRetInt16Ty:
    return getCBufRetType(Type::getInt16Ty(Ctx));
  case OpParamType::CBufRetInt32Ty:
    return getCBufRetType(Type::getInt32Ty(Ctx));
  case OpParamType::CBufRetInt64Ty:
    return getCBufRetType(Type::getInt64Ty(Ctx));
  case OpParamType::HandleTy:
    return getHandleType(Ctx);
  case OpParamType::ResBindTy:
    return getResBindType(Ctx);
  case OpParamType::ResPropsTy:
    return getResPropsType(Ctx);
  case OpParamType::SplitDoubleTy:
    return getSplitDoubleType(Ctx);
  case OpParamType::BinaryWithCarryTy:
    return getBinaryWithCarryType(Ctx);
  }
  llvm_unreachable("Invalid parameter kind");
  return nullptr;
}

static ShaderKind getShaderKindEnum(Triple::EnvironmentType EnvType) {
  switch (EnvType) {
  case Triple::Pixel:
    return ShaderKind::pixel;
  case Triple::Vertex:
    return ShaderKind::vertex;
  case Triple::Geometry:
    return ShaderKind::geometry;
  case Triple::Hull:
    return ShaderKind::hull;
  case Triple::Domain:
    return ShaderKind::domain;
  case Triple::Compute:
    return ShaderKind::compute;
  case Triple::Library:
    return ShaderKind::library;
  case Triple::RayGeneration:
    return ShaderKind::raygeneration;
  case Triple::Intersection:
    return ShaderKind::intersection;
  case Triple::AnyHit:
    return ShaderKind::anyhit;
  case Triple::ClosestHit:
    return ShaderKind::closesthit;
  case Triple::Miss:
    return ShaderKind::miss;
  case Triple::Callable:
    return ShaderKind::callable;
  case Triple::Mesh:
    return ShaderKind::mesh;
  case Triple::Amplification:
    return ShaderKind::amplification;
  default:
    break;
  }
  llvm_unreachable(
      "Shader Kind Not Found - Invalid DXIL Environment Specified");
}

static SmallVector<Type *>
getArgTypesFromOpParamTypes(ArrayRef<dxil::OpParamType> Types,
                            LLVMContext &Context, Type *OverloadTy) {
  SmallVector<Type *> ArgTys;
  ArgTys.emplace_back(Type::getInt32Ty(Context));
  for (dxil::OpParamType Ty : Types)
    ArgTys.emplace_back(getTypeFromOpParamType(Ty, Context, OverloadTy));
  return ArgTys;
}

/// Construct DXIL function type. This is the type of a function with
/// the following prototype
///     OverloadType dx.op.<opclass>.<return-type>(int opcode, <param types>)
/// <param-types> are constructed from types in Prop.
static FunctionType *getDXILOpFunctionType(dxil::OpCode OpCode,
                                           LLVMContext &Context,
                                           Type *OverloadTy) {

  switch (OpCode) {
#define DXIL_OP_FUNCTION_TYPE(OpCode, RetType, ...)                            \
  case OpCode:                                                                 \
    return FunctionType::get(                                                  \
        getTypeFromOpParamType(RetType, Context, OverloadTy),                  \
        getArgTypesFromOpParamTypes({__VA_ARGS__}, Context, OverloadTy),       \
        /*isVarArg=*/false);
#include "DXILOperation.inc"
  }
  llvm_unreachable("Invalid OpCode?");
}

/// Get index of the property from PropList valid for the most recent
/// DXIL version not greater than DXILVer.
/// PropList is expected to be sorted in ascending order of DXIL version.
template <typename T>
static std::optional<size_t> getPropIndex(ArrayRef<T> PropList,
                                          const VersionTuple DXILVer) {
  size_t Index = PropList.size() - 1;
  for (auto Iter = PropList.rbegin(); Iter != PropList.rend();
       Iter++, Index--) {
    const T &Prop = *Iter;
    if (VersionTuple(Prop.DXILVersion.Major, Prop.DXILVersion.Minor) <=
        DXILVer) {
      return Index;
    }
  }
  return std::nullopt;
}

// Helper function to pack an OpCode and VersionTuple into a uint64_t for use
// in a switch statement
constexpr static uint64_t computeSwitchEnum(dxil::OpCode OpCode,
                                            uint16_t VersionMajor,
                                            uint16_t VersionMinor) {
  uint64_t OpCodePack = (uint64_t)OpCode;
  return (OpCodePack << 32) | (VersionMajor << 16) | VersionMinor;
}

// Retreive all the set attributes for a DXIL OpCode given the targeted
// DXILVersion
static dxil::Attributes getDXILAttributes(dxil::OpCode OpCode,
                                          VersionTuple DXILVersion) {
  // Instantiate all versions to iterate through
  SmallVector<Version> Versions = {
#define DXIL_VERSION(MAJOR, MINOR) {MAJOR, MINOR},
#include "DXILOperation.inc"
  };

  dxil::Attributes Attributes;
  for (auto Version : Versions) {
    if (DXILVersion < VersionTuple(Version.Major, Version.Minor))
      continue;

    // Switch through and match an OpCode with the specific version and set the
    // corresponding flag(s) if available
    switch (computeSwitchEnum(OpCode, Version.Major, Version.Minor)) {
#define DXIL_OP_ATTRIBUTES(OpCode, VersionMajor, VersionMinor, ...)            \
  case computeSwitchEnum(OpCode, VersionMajor, VersionMinor): {                \
    auto Other = dxil::Attributes{__VA_ARGS__};                                \
    Attributes |= Other;                                                       \
    break;                                                                     \
  };
#include "DXILOperation.inc"
    }
  }
  return Attributes;
}

// Retreive the set of DXIL Attributes given the version and map them to an
// llvm function attribute that is set onto the instruction
static void setDXILAttributes(CallInst *CI, dxil::OpCode OpCode,
                              VersionTuple DXILVersion) {
  dxil::Attributes Attributes = getDXILAttributes(OpCode, DXILVersion);
  if (Attributes.ReadNone)
    CI->setDoesNotAccessMemory();
  if (Attributes.ReadOnly)
    CI->setOnlyReadsMemory();
  if (Attributes.NoReturn)
    CI->setDoesNotReturn();
  if (Attributes.NoDuplicate)
    CI->setCannotDuplicate();
  return;
}

namespace llvm {
namespace dxil {

// No extra checks on TargetTriple need be performed to verify that the
// Triple is well-formed or that the target is supported since these checks
// would have been done at the time the module M is constructed in the earlier
// stages of compilation.
DXILOpBuilder::DXILOpBuilder(Module &M) : M(M), IRB(M.getContext()) {
  const Triple &TT = M.getTargetTriple();
  DXILVersion = TT.getDXILVersion();
  ShaderStage = TT.getEnvironment();
  // Ensure Environment type is known
  if (ShaderStage == Triple::UnknownEnvironment) {
    reportFatalUsageError(
        Twine(DXILVersion.getAsString()) +
        ": Unknown Compilation Target Shader Stage specified ");
  }
}

static Error makeOpError(dxil::OpCode OpCode, Twine Msg) {
  return make_error<StringError>(
      Twine("Cannot create ") + getOpCodeName(OpCode) + " operation: " + Msg,
      inconvertibleErrorCode());
}

Expected<CallInst *> DXILOpBuilder::tryCreateOp(dxil::OpCode OpCode,
                                                ArrayRef<Value *> Args,
                                                const Twine &Name,
                                                Type *RetTy) {
  const OpCodeProperty *Prop = getOpCodeProperty(OpCode);

  Type *OverloadTy = nullptr;
  if (Prop->OverloadParamIndex == 0) {
    if (!RetTy)
      return makeOpError(OpCode, "Op overloaded on unknown return type");
    OverloadTy = RetTy;
  } else if (Prop->OverloadParamIndex > 0) {
    // The index counts including the return type
    unsigned ArgIndex = Prop->OverloadParamIndex - 1;
    if (static_cast<unsigned>(ArgIndex) >= Args.size())
      return makeOpError(OpCode, "Wrong number of arguments");
    OverloadTy = Args[ArgIndex]->getType();
  }

  FunctionType *DXILOpFT =
      getDXILOpFunctionType(OpCode, M.getContext(), OverloadTy);

  std::optional<size_t> OlIndexOrErr =
      getPropIndex(ArrayRef(Prop->Overloads), DXILVersion);
  if (!OlIndexOrErr.has_value())
    return makeOpError(OpCode, Twine("No valid overloads for DXIL version ") +
                                   DXILVersion.getAsString());

  uint16_t ValidTyMask = Prop->Overloads[*OlIndexOrErr].ValidTys;

  OverloadKind Kind = getOverloadKind(OverloadTy);

  // Check if the operation supports overload types and OverloadTy is valid
  // per the specified types for the operation
  if ((ValidTyMask != OverloadKind::UNDEFINED) &&
      (ValidTyMask & (uint16_t)Kind) == 0)
    return makeOpError(OpCode, "Invalid overload type");

  // Perform necessary checks to ensure Opcode is valid in the targeted shader
  // kind
  std::optional<size_t> StIndexOrErr =
      getPropIndex(ArrayRef(Prop->Stages), DXILVersion);
  if (!StIndexOrErr.has_value())
    return makeOpError(OpCode, Twine("No valid stage for DXIL version ") +
                                   DXILVersion.getAsString());

  uint16_t ValidShaderKindMask = Prop->Stages[*StIndexOrErr].ValidStages;

  // Ensure valid shader stage properties are specified
  if (ValidShaderKindMask == ShaderKind::removed)
    return makeOpError(OpCode, "Operation has been removed");

  // Shader stage need not be validated since getShaderKindEnum() fails
  // for unknown shader stage.

  // Verify the target shader stage is valid for the DXIL operation
  ShaderKind ModuleStagekind = getShaderKindEnum(ShaderStage);
  if (!(ValidShaderKindMask & ModuleStagekind))
    return makeOpError(OpCode, "Invalid stage");

  std::string DXILFnName = constructOverloadName(Kind, OverloadTy, *Prop);
  FunctionCallee DXILFn = M.getOrInsertFunction(DXILFnName, DXILOpFT);

  // We need to inject the opcode as the first argument.
  SmallVector<Value *> OpArgs;
  OpArgs.push_back(IRB.getInt32(llvm::to_underlying(OpCode)));
  OpArgs.append(Args.begin(), Args.end());

  // Create the function call instruction
  CallInst *CI = IRB.CreateCall(DXILFn, OpArgs, Name);

  // We then need to attach available function attributes
  setDXILAttributes(CI, OpCode, DXILVersion);

  return CI;
}

CallInst *DXILOpBuilder::createOp(dxil::OpCode OpCode, ArrayRef<Value *> Args,
                                  const Twine &Name, Type *RetTy) {
  Expected<CallInst *> Result = tryCreateOp(OpCode, Args, Name, RetTy);
  if (Error E = Result.takeError())
    llvm_unreachable("Invalid arguments for operation");
  return *Result;
}

StructType *DXILOpBuilder::getResRetType(Type *ElementTy) {
  return ::getResRetType(ElementTy);
}

StructType *DXILOpBuilder::getCBufRetType(Type *ElementTy) {
  return ::getCBufRetType(ElementTy);
}

StructType *DXILOpBuilder::getHandleType() {
  return ::getHandleType(IRB.getContext());
}

Constant *DXILOpBuilder::getResBind(uint32_t LowerBound, uint32_t UpperBound,
                                    uint32_t SpaceID, dxil::ResourceClass RC) {
  Type *Int32Ty = IRB.getInt32Ty();
  Type *Int8Ty = IRB.getInt8Ty();
  return ConstantStruct::get(
      getResBindType(IRB.getContext()),
      {ConstantInt::get(Int32Ty, LowerBound),
       ConstantInt::get(Int32Ty, UpperBound),
       ConstantInt::get(Int32Ty, SpaceID),
       ConstantInt::get(Int8Ty, llvm::to_underlying(RC))});
}

Constant *DXILOpBuilder::getResProps(uint32_t Word0, uint32_t Word1) {
  Type *Int32Ty = IRB.getInt32Ty();
  return ConstantStruct::get(
      getResPropsType(IRB.getContext()),
      {ConstantInt::get(Int32Ty, Word0), ConstantInt::get(Int32Ty, Word1)});
}

const char *DXILOpBuilder::getOpCodeName(dxil::OpCode DXILOp) {
  return ::getOpCodeName(DXILOp);
}
} // namespace dxil
} // namespace llvm
