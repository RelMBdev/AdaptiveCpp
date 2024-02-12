/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2019-2024 Aksel Alpay
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "hipSYCL/compiler/llvm-to-backend/cpu/LLVMToCpu.hpp"
#include "hipSYCL/common/debug.hpp"
#include "hipSYCL/common/filesystem.hpp"
#include "hipSYCL/compiler/cbs/KernelFlattening.hpp"
#include "hipSYCL/compiler/cbs/PipelineBuilder.hpp"
#include "hipSYCL/compiler/cbs/SimplifyKernel.hpp"
#include "hipSYCL/compiler/cbs/SplitterAnnotationAnalysis.hpp"
#include "hipSYCL/compiler/llvm-to-backend/AddressSpaceInferencePass.hpp"
#include "hipSYCL/compiler/llvm-to-backend/AddressSpaceMap.hpp"
#include "hipSYCL/compiler/llvm-to-backend/Utils.hpp"
#include "hipSYCL/compiler/llvm-to-backend/cpu/HostKernelWrapperPass.hpp"
#include "hipSYCL/compiler/sscp/IRConstantReplacer.hpp"
#include "hipSYCL/glue/llvm-sscp/s2_ir_constants.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cassert>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace hipsycl {
namespace compiler {

LLVMToCpuTranslator::LLVMToCpuTranslator(const std::vector<std::string> &KN)
    : LLVMToBackendTranslator{sycl::sscp::backend::cpu, KN}, KernelNames{KN},
      TargetTriple(llvm::sys::getProcessTriple()), MCpu(std::string(llvm::sys::getHostCPUName())) {}

bool LLVMToCpuTranslator::toBackendFlavor(llvm::Module &M, PassHandler &PH) {

  std::string Triple = llvm::sys::getProcessTriple();
  // Fixme:
  // std::string DataLayout =
  // "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128";

  M.setTargetTriple(Triple);
  // M.setDataLayout(DataLayout);
  // todo: use getHostCPUFeatures or similar to set native features

  AddressSpaceMap ASMap = getAddressSpaceMap();

  for (auto KernelName : KernelNames) {
    if (auto *F = M.getFunction(KernelName)) {

      llvm::SmallVector<llvm::Metadata *, 4> Operands;
      Operands.push_back(llvm::ValueAsMetadata::get(F));
      Operands.push_back(llvm::MDString::get(M.getContext(), "kernel"));
      Operands.push_back(llvm::ValueAsMetadata::getConstant(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(M.getContext()), 1)));

      M.getOrInsertNamedMetadata("hipsycl.sscp.annotations")
          ->addOperand(llvm::MDTuple::get(M.getContext(), Operands));

      F->setLinkage(llvm::GlobalValue::LinkageTypes::ExternalLinkage);
    }
  }

  std::string BuiltinBitcodeFile =
      common::filesystem::join_path(common::filesystem::get_install_directory(),
                                    {"lib", "hipSYCL", "bitcode", "libkernel-sscp-cpu-full.bc"});

  if (!this->linkBitcodeFile(M, BuiltinBitcodeFile))
    return false;

  llvm::ModulePassManager MPM;

  MPM.addPass(AddressSpaceInferencePass{ASMap});

  PH.PassBuilder->registerAnalysisRegistrationCallback([](llvm::ModuleAnalysisManager &MAM) {
    MAM.registerPass([] { return SplitterAnnotationAnalysis{}; });
  });
  PH.PassBuilder->registerModuleAnalyses(*PH.ModuleAnalysisManager);
  registerCBSPipeline(MPM, hipsycl::compiler::OptLevel::O3, true);

  MPM.run(M, *PH.ModuleAnalysisManager);

  return true;
}

bool LLVMToCpuTranslator::translateToBackendFormat(llvm::Module &FlavoredModule, std::string &out) {
  {
    std::error_code EC;
    llvm::raw_fd_ostream rs{"hipsycl-sscp-cpu.ll", EC};
    FlavoredModule.print(rs, nullptr);
  }

  auto InputFile = llvm::sys::fs::TempFile::create("hipsycl-sscp-cpu-%%%%%%.bc");
  auto OutputFile = llvm::sys::fs::TempFile::create("hipsycl-sscp-cpu-%%%%%%.s");

  if (auto E = InputFile.takeError()) {
    this->registerError("LLVMToCpu: Could not create temp file: " + InputFile->TmpName);
    return false;
  }

  if (auto E = OutputFile.takeError()) {
    this->registerError("LLVMToCpu: Could not create temp file: " + OutputFile->TmpName);
    return false;
  }

  std::string OutputFilename = OutputFile->TmpName;

  AtScopeExit DestroyInputFile([&]() {
    if (InputFile->discard())
      ;
  });
  AtScopeExit DestroyOutputFile([&]() {
    if (OutputFile->discard())
      ;
  });

  std::error_code EC;
  llvm::raw_fd_ostream InputStream{InputFile->FD, false};

  llvm::WriteBitcodeToFile(FlavoredModule, InputStream);
  InputStream.flush();

  std::string ClangPath = HIPSYCL_CLANG_PATH;

  llvm::SmallVector<llvm::StringRef, 16> Invocation{
      ClangPath, "-cc1", "-triple",      TargetTriple,      "-O3", "-S", "-x",
      "ir",      "-o",   OutputFilename, InputFile->TmpName};
  if (MCpu != "generic") {
    Invocation.push_back("-target-cpu");
    Invocation.push_back(MCpu);
  }

  std::string ArgString;
  for (const auto &S : Invocation) {
    ArgString += S;
    ArgString += " ";
  }
  HIPSYCL_DEBUG_INFO << "LLVMToCpu: Invoking " << ArgString << "\n";

  int R = llvm::sys::ExecuteAndWait(ClangPath, Invocation);

  if (R != 0) {
    this->registerError("LLVMToCpu: clang invocation failed with exit code " + std::to_string(R));
    return false;
  }

  auto ReadResult = llvm::MemoryBuffer::getFile(OutputFile->TmpName, -1);

  if (auto Err = ReadResult.getError()) {
    this->registerError("LLVMToCpu: Could not read result file" + Err.message());
    return false;
  }

  out = ReadResult->get()->getBuffer();

  return true;
}

bool LLVMToCpuTranslator::applyBuildOption(const std::string &Option, const std::string &Value) {
  if (Option == "triple") {
    this->TargetTriple = Value;
    return true;
  }
  if (Option == "cpu") {
    this->MCpu = Value;
    return true;
  }

  return false;
}

bool LLVMToCpuTranslator::isKernelAfterFlavoring(llvm::Function &F) {
  for (const auto &Name : KernelNames)
    if (F.getName() == Name)
      return true;
  return false;
}

AddressSpaceMap LLVMToCpuTranslator::getAddressSpaceMap() const {
  AddressSpaceMap ASMap;
  // TODO for CPU
  ASMap[AddressSpace::Generic] = 0;
  ASMap[AddressSpace::Global] = 0;
  ASMap[AddressSpace::Local] = 0;
  ASMap[AddressSpace::Private] = 0;
  ASMap[AddressSpace::Constant] = 0;
  // NVVM wants to have allocas in address space 0
  ASMap[AddressSpace::AllocaDefault] = 0;
  ASMap[AddressSpace::GlobalVariableDefault] = 0;
  ASMap[AddressSpace::ConstantGlobalVariableDefault] = 0;

  return ASMap;
}

std::unique_ptr<LLVMToBackendTranslator>
createLLVMToCpuTranslator(const std::vector<std::string> &KernelNames) {
  return std::make_unique<LLVMToCpuTranslator>(KernelNames);
}

} // namespace compiler
} // namespace hipsycl
