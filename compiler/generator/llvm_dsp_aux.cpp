/************************************************************************
 ************************************************************************
    FAUST compiler
	Copyright (C) 2003-2004 GRAME, Centre National de Creation Musicale
    ---------------------------------------------------------------------
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 ************************************************************************
 ************************************************************************/
 
#include <stdio.h>
#include "llvm_dsp_aux.hh"
#include "faust/gui/UIGlue.h"
#include "libfaust.h"

#include <llvm/Support/Threading.h>

#if defined(LLVM_33) || defined(LLVM_34)
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/Support/system_error.h>
#include <llvm/ADT/Triple.h>
#include <llvm/Target/TargetLibraryInfo.h>
#include <llvm/Support/TargetRegistry.h>
#endif

int llvm_dsp_factory::gInstance = 0;
        
void* llvm_dsp_factory::LoadOptimize(const std::string& function)
{
    llvm::Function* fun_ptr = fResult->fModule->getFunction(function);
    if (fun_ptr) {
        return fJIT->getPointerToFunction(fun_ptr);
    } else {
        throw -1;
    }
}

static Module* LoadModule(const std::string filename, LLVMContext* context)
{
    SMDiagnostic err;
    Module* module = ParseIRFile(filename, err, *context);
    
    if (module) {
        return module;
    } else {
     #if defined(LLVM_31) || defined(LLVM_32) || defined(LLVM_33) || defined(LLVM_34)
        err.print("ParseIRFile failed :", errs());
    #else
        err.Print("ParseIRFile failed :", errs());
    #endif
        return 0;
    }
}

LLVMResult* llvm_dsp_factory::CompileModule(int argc, const char *argv[], const char* library_path,  const char* draw_path, const char* input_name, const char* input, char* error_msg)
{
    int argc1 = argc + 3;
 	const char* argv1[32];
	assert(argc1 < 32);
    argv1[0] = "faust";
	argv1[1] = "-lang";
	argv1[2] = "llvm";
    for (int i = 0; i < argc; i++) {
        argv1[i+3] = argv[i];
        if (strcmp(argv[i], "-sch") == 0) {
            fScheduler = true;
        }
    }
    
    fLibraryPath = string(library_path);
    return compile_faust_llvm(argc1, argv1, library_path, draw_path, input_name, input, error_msg);
}

// Bitcode
std::string llvm_dsp_factory::writeDSPFactoryToBitcode()
{
    std::string res;
    raw_string_ostream out(res);
    WriteBitcodeToFile(fResult->fModule, out);
    out.flush();
    return res;
}

void llvm_dsp_factory::writeDSPFactoryToBitcodeFile(const std::string& bit_code_path)
{
    std::string err;
    
#if defined(LLVM_34)
    raw_fd_ostream out(bit_code_path.c_str(), err, sys::fs::F_Binary);
#else
    raw_fd_ostream out(bit_code_path.c_str(), err, raw_fd_ostream::F_Binary);
#endif
    
    WriteBitcodeToFile(fResult->fModule, out);
}

// IR
string llvm_dsp_factory::writeDSPFactoryToIR()
{
    std::string res;
    raw_string_ostream out(res);
    PassManager PM;
    PM.add(createPrintModulePass(&out));
    PM.run(*fResult->fModule);
    out.flush();
    return res;
}

void llvm_dsp_factory::writeDSPFactoryToIRFile(const std::string& ir_code_path)
{
    std::string err;
#if defined(LLVM_34)
    raw_fd_ostream out(ir_code_path.c_str(), err, sys::fs::F_Binary);
#else
    raw_fd_ostream out(ir_code_path.c_str(), err, raw_fd_ostream::F_Binary);
#endif
    PassManager PM;
    PM.add(createPrintModulePass(&out));
    PM.run(*fResult->fModule);
    out.flush();
}

llvm_dsp_factory::llvm_dsp_factory(Module* module, LLVMContext* context, const std::string& target, int opt_level)
{
    fOptLevel = opt_level;
    fTarget = target;
    Init();
    fResult = static_cast<LLVMResult*>(calloc(1, sizeof(LLVMResult)));
    fResult->fModule = module;
    fResult->fContext = context;
}

llvm_dsp_factory::llvm_dsp_factory(int argc, const char *argv[], 
                                    const std::string& library_path,
                                    const std::string& draw_path, 
                                    const std::string& name,
                                    const std::string& input, 
                                    const std::string& target, 
                                    char* error_msg, int opt_level)
{
    if (llvm_dsp_factory::gInstance++ == 0) {
        if (!llvm_start_multithreaded()) {
            printf("llvm_start_multithreaded error...\n");
        }
    }

    fOptLevel = opt_level;
    fTarget = target;
    Init();
    fResult = CompileModule(argc, argv, library_path.c_str(), draw_path.c_str(), name.c_str(), input.c_str(), error_msg);
}

void llvm_dsp_factory::Init()
{
    fJIT = 0;
    fNew = 0;
    fDelete = 0;
    fGetNumInputs = 0;
    fGetNumOutputs = 0;
    fBuildUserInterface = 0;
    fInit = 0;
    fCompute = 0;
    fScheduler = false;
}

llvm_dsp_aux* llvm_dsp_factory::createDSPInstance()
{
    assert(fResult->fModule);
    assert(fJIT);
    return new llvm_dsp_aux(this, fNew());
}

#if defined(LLVM_33) || defined(LLVM_34)

/// AddOptimizationPasses - This routine adds optimization passes
/// based on selected optimization level, OptLevel. This routine
/// duplicates llvm-gcc behaviour.
///
/// OptLevel - Optimization Level
static void AddOptimizationPasses(PassManagerBase &MPM,FunctionPassManager &FPM,
                                    unsigned OptLevel, unsigned SizeLevel) 
{
    FPM.add(createVerifierPass());                  // Verify that input is correct

    PassManagerBuilder Builder;
    Builder.OptLevel = OptLevel;
    Builder.SizeLevel = SizeLevel;

    if (OptLevel > 1) {
        unsigned Threshold = 225;
        if (SizeLevel == 1) {           // -Os
            Threshold = 75;
        } else if (SizeLevel == 2) {    // -Oz
            Threshold = 25;
        }
        if (OptLevel > 2) {
            Threshold = 275;
        }
        Builder.Inliner = createFunctionInliningPass(Threshold);
    } else {
        Builder.Inliner = createAlwaysInlinerPass();
    }
      
    Builder.DisableUnrollLoops = OptLevel == 0;
#if defined(LLVM_33)   
    Builder.DisableSimplifyLibCalls = false;
#endif
      
    if (OptLevel > 3) {
        Builder.LoopVectorize = true;
        Builder.SLPVectorize = true;
    }
    if (OptLevel > 4) {
        Builder.BBVectorize = true;
    }
     
    Builder.populateFunctionPassManager(FPM);
    Builder.populateModulePassManager(MPM);
}

bool llvm_dsp_factory::initJIT(char* error_msg)
{
    // First check is Faust compilation succeeded... (valid LLVM module)
    if (!fResult || !fResult->fModule) {
        return false;
    }
    
    InitializeAllTargets();
    InitializeAllTargetMCs();
  
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    // Initialize passes
    PassRegistry &Registry = *PassRegistry::getPassRegistry();
    
    initializeCodeGen(Registry);
    initializeCore(Registry);
    initializeScalarOpts(Registry);
    initializeObjCARCOpts(Registry);
    initializeVectorization(Registry);
    initializeIPO(Registry);
    initializeAnalysis(Registry);
    initializeIPA(Registry);
    initializeTransformUtils(Registry);
    initializeInstCombine(Registry);
    initializeInstrumentation(Registry);
    initializeTarget(Registry);
    
    std::string err;
    // Link with "scheduler" code
    if (fScheduler) {
        Module* scheduler = LoadModule(fLibraryPath + "scheduler.ll", fResult->fContext);
        if (scheduler) {
            if (Linker::LinkModules(fResult->fModule, scheduler, Linker::DestroySource, &err)) {
                snprintf(error_msg, 256, "Cannot link scheduler module : %s", err.c_str());
                delete scheduler;
                return false;
            } else {
                delete scheduler;
            }
        } else {
            strncpy(error_msg, "File scheduler.ll not found...", 256);
            return false;
        }
    }
    
    if (fTarget != "") {
        fResult->fModule->setTargetTriple(fTarget);
    } else {
        fResult->fModule->setTargetTriple(llvm::sys::getDefaultTargetTriple());
    }

    EngineBuilder builder(fResult->fModule);
    builder.setOptLevel(CodeGenOpt::Aggressive);
    builder.setEngineKind(EngineKind::JIT);
    // MCJIT does not work correctly (incorrect float numbers ?) when used with dynamic libLLVM
    builder.setUseMCJIT(true);
    //builder.setUseMCJIT(false);
    builder.setCodeModel(CodeModel::JITDefault);
    builder.setMCPU(llvm::sys::getHostCPUName());
    
    TargetOptions targetOptions;
    targetOptions.NoFramePointerElim = true;
    targetOptions.LessPreciseFPMADOption = true;
    targetOptions.UnsafeFPMath = true;
    targetOptions.NoInfsFPMath = true;
    targetOptions.NoNaNsFPMath = true;
    targetOptions.GuaranteedTailCallOpt = true;
     
    string debug_var = (getenv("FAUST_DEBUG")) ? string(getenv("FAUST_DEBUG")) : "";
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM3") != string::npos)) {
       targetOptions.PrintMachineCode = true;
    }
    
    builder.setTargetOptions(targetOptions);
    TargetMachine* tm = builder.selectTarget();
    
    PassManager pm;
    FunctionPassManager fpm(fResult->fModule);
  
    // Add an appropriate TargetLibraryInfo pass for the module's triple.
    TargetLibraryInfo* tli = new TargetLibraryInfo(Triple(fResult->fModule->getTargetTriple()));
    pm.add(tli);
    
    const std::string &ModuleDataLayout = fResult->fModule->getDataLayout();
    DataLayout* td = new DataLayout(ModuleDataLayout);
    pm.add(td);
  
    // Add internal analysis passes from the target machine (mandatory for vectorization to work)
    tm->addAnalysisPasses(pm);
    
    if (fOptLevel > 0) {
        AddOptimizationPasses(pm, fpm, fOptLevel, 0);
    }
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM1") != string::npos)) {
        fResult->fModule->dump();
    }
   
    fpm.doInitialization();
    for (Module::iterator F = fResult->fModule->begin(), E = fResult->fModule->end(); F != E; ++F) {
        fpm.run(*F);
    }
    fpm.doFinalization();
    
    pm.add(createVerifierPass());
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM4") != string::npos)) {
        tm->addPassesToEmitFile(pm, fouts(), TargetMachine::CGFT_AssemblyFile, true);
    }
    
    // Now that we have all of the passes ready, run them.
    pm.run(*fResult->fModule);
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM2") != string::npos)) {
        fResult->fModule->dump();
    }
    
    fJIT = builder.create(tm);
    if (!fJIT) {
        return false;
    }
    
    // Run static constructors.
    fJIT->runStaticConstructorsDestructors(false);
    fJIT->DisableLazyCompilation(true);
    
    try {
        fNew = (newDspFun)LoadOptimize("new_mydsp");
        fDelete = (deleteDspFun)LoadOptimize("delete_mydsp");
        fGetNumInputs = (getNumInputsFun)LoadOptimize("getNumInputs_mydsp");
        fGetNumOutputs = (getNumOutputsFun)LoadOptimize("getNumOutputs_mydsp");
        fBuildUserInterface = (buildUserInterfaceFun)LoadOptimize("buildUserInterface_mydsp");
        fInit = (initFun)LoadOptimize("init_mydsp");
        fCompute = (computeFun)LoadOptimize("compute_mydsp");
        fMetadata = (metadataFun)LoadOptimize("metadata_mydsp");
        return true;
    } catch (...) { // Module does not contain the Faust entry points...
        return false;
    }
}

#else

bool llvm_dsp_factory::initJIT(char* error_msg)
{
    // First check is Faust compilation succeeded... (valid LLVM module)
    if (!fResult || !fResult->fModule) {
        return false;
    }
    
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();
    
    if (fTarget != "") {
         fResult->fModule->setTargetTriple(fTarget);
    } else {
    #if defined(LLVM_31) || defined(LLVM_32) 
        fResult->fModule->setTargetTriple(llvm::sys::getDefaultTargetTriple());
    #else
        fResult->fModule->setTargetTriple(llvm::sys::getHostTriple());
    #endif
    }

    std::string err;
    EngineBuilder builder(fResult->fModule);
    builder.setOptLevel(CodeGenOpt::Aggressive);
    builder.setEngineKind(EngineKind::JIT);
    // MCJIT does not work correctly (incorrect float numbers ?) when used with dynamic libLLVM
    //builder.setUseMCJIT(true);
    builder.setUseMCJIT(false);
    builder.setMCPU(llvm::sys::getHostCPUName());
 
       
#ifndef LLVM_30
    TargetMachine* tm = builder.selectTarget();
#endif
    //tm->Options.PrintMachineCode = 1;
    /*
    SmallVector<std::string, 4> attrs;
    attrs.push_back("sse");
    attrs.push_back("sse2");
    attrs.push_back("sse3");
    attrs.push_back("enable-unsafe-fp-math");
    builder.setMAttrs(attrs);
    */
#ifdef LLVM_30
    fJIT = builder.create();
#else
    fJIT = builder.create(tm);
#endif
    if (!fJIT) {
        return false;
    }
    
    // Run static constructors.
    fJIT->runStaticConstructorsDestructors(false);
    
    fJIT->DisableLazyCompilation(true);
#if defined(LLVM_32) 
    fResult->fModule->setDataLayout(fJIT->getDataLayout()->getStringRepresentation());
#else
    fResult->fModule->setDataLayout(fJIT->getTargetData()->getStringRepresentation());
#endif
  
    // Set up the optimizer pipeline. Start with registering info about how the
    // target lays out data structures.
    PassManager pm;
    FunctionPassManager fpm(fResult->fModule);
#if defined(LLVM_32)    
    // TODO
#else
    pm.add(new TargetData(*fJIT->getTargetData()));
    fpm.add(new TargetData(*fJIT->getTargetData()));
#endif

    // Link with "scheduler" code
    if (fScheduler) {
        Module* scheduler = LoadModule(fLibraryPath + "scheduler.ll", fResult->fContext);
        if (scheduler) {
            if (Linker::LinkModules(fResult->fModule, scheduler, Linker::DestroySource, &err)) {
                snprintf(error_msg, 256, "Cannot link scheduler module : %s", err.c_str());
                delete scheduler;
                return false;
            } else {
                delete scheduler;
            }
        } else {
            strncpy(error_msg, "File scheduler.ll not found...", 256);
            return false;
        }
    }
    
    // Taken from LLVM Opt.cpp
    PassManagerBuilder Builder;
    Builder.OptLevel = fOptLevel;

    if (fOptLevel > 1) {
        unsigned threshold = 225;
        if (fOptLevel > 2) {
            threshold = 275;
        }
        Builder.Inliner = createFunctionInliningPass(threshold);
    } else {
        Builder.Inliner = createAlwaysInlinerPass();
    }
    
    // We use '4' to activate de auto-vectorizer
    if (fOptLevel > 3) {
    
    #if defined(LLVM_32) 
        printf("Vectorize\n");
        Builder.LoopVectorize = true;
        //Builder.Vectorize = true;
    #elif defined(LLVM_31)
        Builder.Vectorize = true;
    #endif
    }
      
    Builder.DisableUnrollLoops = (fOptLevel == 0);
    Builder.populateFunctionPassManager(fpm);
    Builder.populateModulePassManager(pm);
    
    string debug_var = (getenv("FAUST_DEBUG")) ? string(getenv("FAUST_DEBUG")) : "";
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM1") != string::npos)) {
        fResult->fModule->dump();
    }
    
    pm.run(*fResult->fModule);
    
    if ((debug_var != "") && (debug_var.find("FAUST_LLVM2") != string::npos)) {
        fResult->fModule->dump();
    }
    
    try {
        fNew = (newDspFun)LoadOptimize("new_mydsp");
        fDelete = (deleteDspFun)LoadOptimize("delete_mydsp");
        fGetNumInputs = (getNumInputsFun)LoadOptimize("getNumInputs_mydsp");
        fGetNumOutputs = (getNumOutputsFun)LoadOptimize("getNumOutputs_mydsp");
        fBuildUserInterface = (buildUserInterfaceFun)LoadOptimize("buildUserInterface_mydsp");
        fInit = (initFun)LoadOptimize("init_mydsp");
        fCompute = (computeFun)LoadOptimize("compute_mydsp");
        fMetadata = (metadataFun)LoadOptimize("metadata_mydsp");
        return true;
    } catch (...) { // Module does not contain the Faust entry points...
        return false;
    }
}

#endif

llvm_dsp_factory::~llvm_dsp_factory()
{
    if (fJIT) {
        fJIT->runStaticConstructorsDestructors(true);
        // fResult->fModule is kept and deleted by fJIT
        delete fJIT;
    }
    
    if (fResult) {
        delete fResult->fContext;
        free(fResult);
    }
    
    if (--llvm_dsp_factory::gInstance == 0) {
        llvm_stop_multithreaded();
    }
}

void llvm_dsp_factory::metadataDSPFactory(Meta* meta)
{
    MetaGlue glue;
    buildMetaGlue(&glue, meta);
    fMetadata(&glue);
}

void llvm_dsp_factory::metadataDSPFactory(MetaGlue* glue)
{
    fMetadata(glue);
}
  
// Instance 

llvm_dsp_aux::llvm_dsp_aux(llvm_dsp_factory* factory, llvm_dsp_imp* dsp)
    :fDSPFactory(factory), fDSP(dsp)
{
    assert(fDSPFactory);
    assert(fDSP);
}
        
llvm_dsp_aux::~llvm_dsp_aux()
{   
    if (fDSP) {
        fDSPFactory->fDelete(fDSP);
    }
}

int llvm_dsp_aux::getNumInputs()
{
    return fDSPFactory->fGetNumInputs(fDSP);
}
int llvm_dsp_aux::getNumOutputs()
{
    return fDSPFactory->fGetNumOutputs(fDSP);
}

void llvm_dsp_aux::init(int samplingFreq)
{
    fDSPFactory->fInit(fDSP, samplingFreq);
}

void llvm_dsp_aux::buildUserInterface(UI* interface)
{
    UIGlue glue;
    buildUIGlue(&glue, interface);
    fDSPFactory->fBuildUserInterface(fDSP, &glue);
}

void llvm_dsp_aux::buildUserInterface(UIGlue* glue)
{
    fDSPFactory->fBuildUserInterface(fDSP, glue);
}

void llvm_dsp_aux::compute(int count, FAUSTFLOAT** input, FAUSTFLOAT** output)
{
    AVOIDDENORMALS;
    fDSPFactory->fCompute(fDSP, count, input, output);
}

static llvm_dsp_factory* CheckDSPFactory(llvm_dsp_factory* factory, char* error_msg)
{
    if (factory->initJIT(error_msg)) {
        return factory;
    } else {
        delete factory;
        return 0;
    }
}

// Public API

EXPORT llvm_dsp_factory* createDSPFactory(int argc, const char *argv[], 
    const std::string& library_path, const std::string& draw_path, const std::string& name, 
    const std::string& input, const std::string& target, 
    char* error_msg, int opt_level)
{
    return CheckDSPFactory(new llvm_dsp_factory(argc, argv, library_path, draw_path, name, input, target, error_msg, opt_level), error_msg);
}
    
// Bitcode <==> string
EXPORT llvm_dsp_factory* readDSPFactoryFromBitcode(const std::string& bit_code, const std::string& target, int opt_level)
{
    string error_msg;
    MemoryBuffer* buffer = MemoryBuffer::getMemBuffer(StringRef(bit_code));
    LLVMContext* context = new LLVMContext();
    Module* module = ParseBitcodeFile(buffer, *context, &error_msg);
    delete buffer;
    
    if (module) {
        char error_msg[256];
        return CheckDSPFactory(new llvm_dsp_factory(module, context, target, opt_level), error_msg);
    } else {
        printf("readDSPFactoryFromBitcode failed : %s\n", error_msg.c_str());
        delete context;
        return 0;
    }
}

EXPORT std::string writeDSPFactoryToBitcode(llvm_dsp_factory* factory)
{
    return factory->writeDSPFactoryToBitcode();
}

// Bitcode <==> file
EXPORT llvm_dsp_factory* readDSPFactoryFromBitcodeFile(const std::string& bit_code_path, const std::string& target, int opt_level)
{
    OwningPtr<MemoryBuffer> buffer;
    if (llvm::error_code ec = MemoryBuffer::getFileOrSTDIN(bit_code_path.c_str(), buffer)) {
        printf("readDSPFactoryFromBitcodeFile failed : %s\n", ec.message().c_str());
        return 0;
    }
  
    std::string error_msg;
    LLVMContext* context = new LLVMContext();
    Module* module = ParseBitcodeFile(buffer.get(), *context, &error_msg);
    
    if (module) {
        char error_msg[256];
        return CheckDSPFactory(new llvm_dsp_factory(module, context, target, opt_level), error_msg);
    } else {
        printf("readDSPFactoryFromBitcodeFile failed : %s\n", error_msg.c_str());
        delete context;
        return 0;
    }
}

EXPORT void writeDSPFactoryToBitcodeFile(llvm_dsp_factory* factory, const std::string& bit_code_path)
{
    factory->writeDSPFactoryToBitcodeFile(bit_code_path);
}

// IR <==> string
EXPORT llvm_dsp_factory* readDSPFactoryFromIR(const std::string& ir_code, const std::string& target, int opt_level)
{
    SMDiagnostic err;
    char* tmp_local = setlocale(LC_ALL, NULL);
    setlocale(LC_ALL, "C");
    MemoryBuffer* buffer = MemoryBuffer::getMemBuffer(StringRef(ir_code));
    LLVMContext* context = new LLVMContext();
    Module* module = ParseIR(buffer, err, *context); // ParseIR takes ownership of the given buffer, so don't delete it
    setlocale(LC_ALL, tmp_local);
    
    if (module) {
        char error_msg[256];
        return CheckDSPFactory(new llvm_dsp_factory(module, context, target, opt_level), error_msg);
    } else {
    #if defined(LLVM_31) || defined(LLVM_32) || defined(LLVM_33) || defined(LLVM_34)
        err.print("readDSPFactoryFromIR failed :", errs());
    #else
        err.Print("readDSPFactoryFromIR failed :", errs());
    #endif
        delete context;
        return 0;
    }
}

EXPORT std::string writeDSPFactoryToIR(llvm_dsp_factory* factory)
{
    return factory->writeDSPFactoryToIR();
}

// IR <==> file
EXPORT llvm_dsp_factory* readDSPFactoryFromIRFile(const std::string& ir_code_path, const std::string& target, int opt_level)
{
    SMDiagnostic err;
    char* tmp_local = setlocale(LC_ALL, NULL);
    setlocale(LC_ALL, "C");
    LLVMContext* context = new LLVMContext();
    Module* module = ParseIRFile(ir_code_path, err, *context);
    setlocale(LC_ALL, tmp_local);
    
    if (module) {
        char error_msg[256];
        return CheckDSPFactory(new llvm_dsp_factory(module, context, target, opt_level), error_msg);
    } else {
    #if defined(LLVM_31) || defined(LLVM_32) || defined(LLVM_33) || defined(LLVM_34)
        err.print("readDSPFactoryFromIR failed :", errs());
    #else
        err.Print("readDSPFactoryFromIR failed :", errs());
    #endif
        delete context;
        return 0;
    }
}

EXPORT void writeDSPFactoryToIRFile(llvm_dsp_factory* factory, const std::string& ir_code_path)
{
    factory->writeDSPFactoryToIRFile(ir_code_path);
}

EXPORT void metadataDSPFactory(llvm_dsp_factory* factory, Meta* m)
{
    factory->metadataDSPFactory(m);
}

// Instance

EXPORT llvm_dsp* createDSPInstance(llvm_dsp_factory* factory)
{
    return reinterpret_cast<llvm_dsp*>(factory->createDSPInstance());
}

EXPORT void deleteDSPFactory(llvm_dsp_factory* factory) { delete factory; }

EXPORT void deleteDSPInstance(llvm_dsp* dsp) 
{
    delete reinterpret_cast<llvm_dsp_aux*>(dsp); 
}

EXPORT int llvm_dsp::getNumInputs()
{
    return reinterpret_cast<llvm_dsp_aux*>(this)->getNumInputs();
}

int EXPORT llvm_dsp::getNumOutputs()
{
    return reinterpret_cast<llvm_dsp_aux*>(this)->getNumOutputs();
}

EXPORT void llvm_dsp::init(int samplingFreq)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->init(samplingFreq);
}

EXPORT void llvm_dsp::buildUserInterface(UI* interface)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->buildUserInterface(interface);
}

EXPORT void llvm_dsp::compute(int count, FAUSTFLOAT** input, FAUSTFLOAT** output)
{
    reinterpret_cast<llvm_dsp_aux*>(this)->compute(count, input, output);
}

// Public C interface

EXPORT llvm_dsp_factory* createCDSPFactory(int argc, const char *argv[], 
                                        const char* library_path, const char* draw_path, 
                                        const char* name, const char* input, 
                                        const char* target, char* error_msg, int opt_level)
{
    return CheckDSPFactory(new llvm_dsp_factory(argc, argv, library_path, draw_path, name, input, target, error_msg, opt_level), error_msg);
}

EXPORT void deleteCDSPFactory(llvm_dsp_factory* factory)
{
    delete factory;
}

EXPORT llvm_dsp_factory* readCDSPFactoryFromBitcode(const char* bit_code, const char* target, int opt_level)
{
    return readDSPFactoryFromBitcode(bit_code, target, opt_level);
}

EXPORT const char* writeCDSPFactoryToBitcode(llvm_dsp_factory* factory)
{
    return writeDSPFactoryToBitcode(factory).c_str();
}

EXPORT llvm_dsp_factory* readCDSPFactoryFromBitcodeFile(const char* bit_code_path, const char* target, int opt_level)
{
    return readDSPFactoryFromBitcodeFile(bit_code_path, target, opt_level);
}

EXPORT void writeCDSPFactoryToBitcodeFile(llvm_dsp_factory* factory, const char* bit_code_path)
{
    writeDSPFactoryToBitcodeFile(factory, bit_code_path);
}

EXPORT llvm_dsp_factory* readCDSPFactoryFromIR(const char* ir_code, const char* target, int opt_level)
{
    return readDSPFactoryFromIR(ir_code, target, opt_level);
}

EXPORT const char* writeCDSPFactoryToIR(llvm_dsp_factory* factory)
{
    return writeDSPFactoryToIR(factory).c_str();
}

EXPORT llvm_dsp_factory* readCDSPFactoryFromIRFile(const char* ir_code_path, const char* target, int opt_level)
{
    return readDSPFactoryFromIRFile(ir_code_path, target, opt_level);
}

EXPORT void writeCDSPFactoryToIRFile(llvm_dsp_factory* factory, const char* ir_code_path)
{
    writeDSPFactoryToIRFile(factory, ir_code_path);
}

EXPORT void metadataCDSPFactory(llvm_dsp_factory* factory, MetaGlue* glue)
{
    factory->metadataDSPFactory(glue);
}

EXPORT int getNumInputsCDSPInstance(llvm_dsp* dsp)
{
    return reinterpret_cast<llvm_dsp_aux*>(dsp)->getNumInputs();
}

EXPORT int getNumOutputsCDSPInstance(llvm_dsp* dsp)
{
    return reinterpret_cast<llvm_dsp_aux*>(dsp)->getNumOutputs();
}

EXPORT void initCDSPInstance(llvm_dsp* dsp, int samplingFreq)
{
    reinterpret_cast<llvm_dsp_aux*>(dsp)->init(samplingFreq);
}

EXPORT void buildUserInterfaceCDSPInstance(llvm_dsp* dsp, UIGlue* glue)
{
    reinterpret_cast<llvm_dsp_aux*>(dsp)->buildUserInterface(glue);
}

EXPORT void computeCDSPInstance(llvm_dsp* dsp, int count, FAUSTFLOAT** input, FAUSTFLOAT** output)
{
    reinterpret_cast<llvm_dsp_aux*>(dsp)->compute(count, input, output);
}

EXPORT llvm_dsp* createCDSPInstance(llvm_dsp_factory* factory)
{
    return reinterpret_cast<llvm_dsp*>(factory->createDSPInstance());
}

EXPORT void deleteCDSPInstance(llvm_dsp* dsp)
{
    delete reinterpret_cast<llvm_dsp_aux*>(dsp); 
}