#include "LlvmCompiler.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/CodeGen/CommandFlags.inc"
#include "llvm/CodeGen/LinkAllAsmWriterComponents.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/CodeGen/MIRParser/MIRParser.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/AutoUpgrade.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <memory>
using namespace llvm;

// General options for llc.  Other pass-specific options are specified
// within the corresponding llc passes, and target-specific options
// and back-end code generation options are specified with the target machine.
//
static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::init("-"));

static cl::opt<std::string>
InputLanguage("x", cl::desc("Input language ('ir' or 'mir')"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));

static cl::opt<std::string>
SplitDwarfOutputFile("split-dwarf-output",
    cl::desc(".dwo output filename"),
    cl::value_desc("filename"));

static cl::opt<unsigned>
TimeCompilations("time-compilations", cl::Hidden, cl::init(1u),
    cl::value_desc("N"),
    cl::desc("Repeat compilation N times for timing"));

static cl::opt<bool>
NoIntegratedAssembler("no-integrated-as", cl::Hidden,
    cl::desc("Disable integrated assembler"));

static cl::opt<bool>
PreserveComments("preserve-as-comments", cl::Hidden,
    cl::desc("Preserve Comments in outputted assembly"),
    cl::init(true));

// Determine optimization level.
static cl::opt<char>
OptLevel("O",
    cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
        "(default = '-O2')"),
    cl::Prefix,
    cl::ZeroOrMore,
    cl::init(' '));

static cl::opt<std::string>
TargetTriple("mtriple", cl::desc("Override target triple for module"));

static cl::opt<std::string> SplitDwarfFile(
    "split-dwarf-file",
    cl::desc(
        "Specify the name of the .dwo file to encode in the DWARF output"));

static cl::opt<bool> NoVerify("disable-verify", cl::Hidden,
    cl::desc("Do not verify input module"));

static cl::opt<bool> DisableSimplifyLibCalls("disable-simplify-libcalls",
    cl::desc("Disable simplify-libcalls"));

static cl::opt<bool> ShowMCEncoding("show-mc-encoding", cl::Hidden,
    cl::desc("Show encoding in .s output"));

static cl::opt<bool> EnableDwarfDirectory(
    "enable-dwarf-directory", cl::Hidden,
    cl::desc("Use .file directives with an explicit directory."));

static cl::opt<bool> AsmVerbose("asm-verbose",
    cl::desc("Add comments to directives."),
    cl::init(true));

static cl::opt<bool>
CompileTwice("compile-twice", cl::Hidden,
    cl::desc("Run everything twice, re-using the same pass "
        "manager and verify the result is the same."),
    cl::init(false));

static cl::opt<bool> DiscardValueNames(
    "discard-value-names",
    cl::desc("Discard names from Value (other than GlobalValue)."),
    cl::init(false), cl::Hidden);

static cl::list<std::string> IncludeDirs("I", cl::desc("include search path"));

static cl::opt<bool> PassRemarksWithHotness(
    "pass-remarks-with-hotness",
    cl::desc("With PGO, include profile count in optimization remarks"),
    cl::Hidden);

static cl::opt<unsigned> PassRemarksHotnessThreshold(
    "pass-remarks-hotness-threshold",
    cl::desc("Minimum profile count required for an optimization remark to be output"),
    cl::Hidden);

static cl::opt<std::string>
RemarksFilename("pass-remarks-output",
    cl::desc("YAML output filename for pass remarks"),
    cl::value_desc("filename"));

namespace {
    static ManagedStatic<std::vector<std::string>> RunPassNames;

    struct RunPassOption {
        void operator=(const std::string &Val) const {
            if (Val.empty())
                return;
            SmallVector<StringRef, 8> PassNames;
            StringRef(Val).split(PassNames, ',', -1, false);
            for (auto PassName : PassNames)
                RunPassNames->push_back(PassName);
        }
    };
}

static RunPassOption RunPassOpt;

static cl::opt<RunPassOption, true, cl::parser<std::string>> RunPass(
    "run-pass",
    cl::desc("Run compiler only for specified passes (comma separated list)"),
    cl::value_desc("pass-name"), cl::ZeroOrMore, cl::location(RunPassOpt));

static int compileModule(char *, LLVMContext &);

static std::unique_ptr<ToolOutputFile> GetOutputStream(void) {

    assert(!OutputFilename.empty());
    assert(FileType == TargetMachine::CGFT_ObjectFile);

    bool Binary = true;

    // Open the file.
    std::error_code EC;
    sys::fs::OpenFlags OpenFlags = sys::fs::F_None;
    auto FDOut = llvm::make_unique<ToolOutputFile>(OutputFilename, EC, OpenFlags);
    if (EC) {
        WithColor::error() << EC.message() << '\n';
        return nullptr;
    }

    return FDOut;
}

struct LLCDiagnosticHandler : public DiagnosticHandler {
    bool *HasError;
    LLCDiagnosticHandler(bool *HasErrorPtr) : HasError(HasErrorPtr) {}
    bool handleDiagnostics(const DiagnosticInfo &DI) override {
        if (DI.getSeverity() == DS_Error)
            *HasError = true;

        if (auto *Remark = dyn_cast<DiagnosticInfoOptimizationBase>(&DI))
            if (!Remark->isEnabled())
                return true;

        DiagnosticPrinterRawOStream DP(errs());
        errs() << LLVMContext::getDiagnosticMessagePrefix(DI.getSeverity()) << ": ";
        DI.print(DP);
        errs() << "\n";
        return true;
    }
};

static void InlineAsmDiagHandler(const SMDiagnostic &SMD, void *Context,
    unsigned LocCookie) {
    bool *HasError = static_cast<bool *>(Context);
    if (SMD.getKind() == SourceMgr::DK_Error)
        *HasError = true;

    SMD.print(nullptr, errs());

    // For testing purposes, we print the LocCookie here.
    if (LocCookie)
        WithColor::note() << "!srcloc = " << LocCookie << "\n";
}

// Compile .ll or .bc to .obj
int llvm_compile(char * progName, char * input, char * output) {
    // Enable debug stream buffering.
    EnableDebugBuffering = true;

    LLVMContext Context;

    // Initialize targets first, so that --version shows registered targets.
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmPrinters();
    InitializeAllAsmParsers();

    // Initialize codegen and IR passes used by llc so that the -print-after,
    // -print-before, and -stop-after options work.
    PassRegistry *Registry = PassRegistry::getPassRegistry();
    initializeCore(*Registry);
    initializeCodeGen(*Registry);
    initializeLoopStrengthReducePass(*Registry);
    initializeLowerIntrinsicsPass(*Registry);
    initializeEntryExitInstrumenterPass(*Registry);
    initializePostInlineEntryExitInstrumenterPass(*Registry);
    initializeUnreachableBlockElimLegacyPassPass(*Registry);
    initializeConstantHoistingLegacyPassPass(*Registry);
    initializeScalarOpts(*Registry);
    initializeVectorization(*Registry);
    initializeScalarizeMaskedMemIntrinPass(*Registry);
    initializeExpandReductionsPass(*Registry);

    // Initialize debugging passes.
    initializeScavengerTestPass(*Registry);

    // Register the target printer for --version.
    cl::AddExtraVersionPrinter(TargetRegistry::printRegisteredTargetsForVersion);

    FileType = TargetMachine::CGFT_ObjectFile;
    InputFilename = std::string(input);
    OutputFilename = std::string(output);
#ifdef _M_IX86
 	MArch = std::string("x86");
#else
	MArch = std::string("x86-64");
#endif

    Context.setDiscardValueNames(DiscardValueNames);

    // Set a diagnostic handler that doesn't exit on the first error
    bool HasError = false;
    Context.setDiagnosticHandler(
        llvm::make_unique<LLCDiagnosticHandler>(&HasError));
    Context.setInlineAsmDiagnosticHandler(InlineAsmDiagHandler, &HasError);

    if (PassRemarksWithHotness)
        Context.setDiagnosticsHotnessRequested(true);

    if (PassRemarksHotnessThreshold)
        Context.setDiagnosticsHotnessThreshold(PassRemarksHotnessThreshold);

    std::unique_ptr<ToolOutputFile> YamlFile;
    if (RemarksFilename != "") {
        std::error_code EC;
        YamlFile =
            llvm::make_unique<ToolOutputFile>(RemarksFilename, EC, sys::fs::F_None);
        if (EC) {
            WithColor::error(errs(), progName) << EC.message() << '\n';
            return 1;
        }
        Context.setDiagnosticsOutputFile(
            llvm::make_unique<yaml::Output>(YamlFile->os()));
    }

    if (InputLanguage != "" && InputLanguage != "ir" &&
        InputLanguage != "mir") {
        WithColor::error(errs(), progName)
            << "input language must be '', 'IR' or 'MIR'\n";
        return 1;
    }

    if (int RetVal = compileModule(progName, Context))
        return RetVal;

    if (YamlFile)
        YamlFile->keep();

    return 0;
}

static bool addPass(PassManagerBase &PM, const char *progName,
    StringRef PassName, TargetPassConfig &TPC) {
    if (PassName == "none")
        return false;

    const PassRegistry *PR = PassRegistry::getPassRegistry();
    const PassInfo *PI = PR->getPassInfo(PassName);
    if (!PI) {
        WithColor::error(errs(), progName)
            << "run-pass " << PassName << " is not registered.\n";
        return true;
    }

    Pass *P;
    if (PI->getNormalCtor())
        P = PI->getNormalCtor()();
    else {
        WithColor::error(errs(), progName)
            << "cannot create pass: " << PI->getPassName() << "\n";
        return true;
    }
    std::string Banner = std::string("After ") + std::string(P->getPassName());
    PM.add(P);
    TPC.printAndVerify(Banner);

    return false;
}

static int compileModule(char * progName, LLVMContext &Context) {
    // Load the module to be compiled...
    SMDiagnostic Err;
    std::unique_ptr<Module> M;
    std::unique_ptr<MIRParser> MIR;
    Triple TheTriple;

    M = parseIRFile(InputFilename, Err, Context, false);

    if (!M) {
        Err.print(progName, WithColor::error(errs(), progName));
        return 1;
    }

    // If we are supposed to override the target triple, do so now.
    assert(TargetTriple.empty());
    if (!TargetTriple.empty())
        M->setTargetTriple(Triple::normalize(TargetTriple));

    TheTriple = Triple(M->getTargetTriple());

    if (TheTriple.getTriple().empty())
        TheTriple.setTriple(sys::getDefaultTargetTriple());

    // Get the target specific parser.
    std::string Error;
    const Target *TheTarget = TargetRegistry::lookupTarget(MArch, TheTriple,
        Error);
    if (!TheTarget) {
        WithColor::error(errs(), progName) << Error;
        return 1;
    }

    std::string CPUStr = getCPUStr(), FeaturesStr = getFeaturesStr();

    CodeGenOpt::Level OLvl = CodeGenOpt::Default;
    switch (OptLevel) {
    default:
        WithColor::error(errs(), progName) << "invalid optimization level.\n";
        return 1;
    case ' ': break;
    case '0': OLvl = CodeGenOpt::None; break;
    case '1': OLvl = CodeGenOpt::Less; break;
    case '2': OLvl = CodeGenOpt::Default; break;
    case '3': OLvl = CodeGenOpt::Aggressive; break;
    }

    TargetOptions Options = InitTargetOptionsFromCodeGenFlags();
    Options.DisableIntegratedAS = NoIntegratedAssembler;
    Options.MCOptions.ShowMCEncoding = ShowMCEncoding;
    Options.MCOptions.MCUseDwarfDirectory = EnableDwarfDirectory;
    Options.MCOptions.AsmVerbose = AsmVerbose;
    Options.MCOptions.PreserveAsmComments = PreserveComments;
    Options.MCOptions.IASSearchPaths = IncludeDirs;
    Options.MCOptions.SplitDwarfFile = SplitDwarfFile;

    std::unique_ptr<TargetMachine> Target(TheTarget->createTargetMachine(
        TheTriple.getTriple(), CPUStr, FeaturesStr, Options, getRelocModel(),
        getCodeModel(), OLvl));

    assert(Target && "Could not allocate target machine!");

    assert(M && "Should have exited if we didn't have a module!");
    if (FloatABIForCalls != FloatABI::Default)
        Options.FloatABIType = FloatABIForCalls;

    // Figure out where we are going to send the output.
    std::unique_ptr<ToolOutputFile> Out = GetOutputStream();
    if (!Out) return 1;

    std::unique_ptr<ToolOutputFile> DwoOut;
    if (!SplitDwarfOutputFile.empty()) {
        std::error_code EC;
        DwoOut = llvm::make_unique<ToolOutputFile>(SplitDwarfOutputFile, EC,
            sys::fs::F_None);
        if (EC) {
            WithColor::error(errs(), progName) << EC.message() << '\n';
            return 1;
        }
    }

    // Build up all of the passes that we want to do to the module.
    legacy::PassManager PM;

    // Add an appropriate TargetLibraryInfo pass for the module's triple.
    TargetLibraryInfoImpl TLII(Triple(M->getTargetTriple()));

    // The -disable-simplify-libcalls flag actually disables all builtin optzns.
    if (DisableSimplifyLibCalls)
        TLII.disableAllFunctions();
    PM.add(new TargetLibraryInfoWrapperPass(TLII));

    // Add the target data from the target machine, if it exists, or the module.
    M->setDataLayout(Target->createDataLayout());

    // This needs to be done after setting datalayout since it calls verifier
    // to check debug info whereas verifier relies on correct datalayout.
    UpgradeDebugInfo(*M);

    // Verify module immediately to catch problems before doInitialization() is
    // called on any passes.
    if (!NoVerify && verifyModule(*M, &errs())) {
        std::string Prefix =
            (Twine(progName) + Twine(": ") + Twine(InputFilename)).str();
        WithColor::error(errs(), Prefix) << "input module is broken!\n";
        return 1;
    }

    // Override function attributes based on CPUStr, FeaturesStr, and command line
    // flags.
    setFunctionAttributes(CPUStr, FeaturesStr, *M);

    if (RelaxAll.getNumOccurrences() > 0 &&
        FileType != TargetMachine::CGFT_ObjectFile)
        WithColor::warning(errs(), progName)
        << ": warning: ignoring -mc-relax-all because filetype != obj";

    {
        raw_pwrite_stream *OS = &Out->os();

        // Manually do the buffering rather than using buffer_ostream,
        // so we can memcmp the contents in CompileTwice mode
        SmallVector<char, 0> Buffer;
        std::unique_ptr<raw_svector_ostream> BOS;
        if ((FileType != TargetMachine::CGFT_AssemblyFile &&
            !Out->os().supportsSeeking()) ||
            CompileTwice) {
            BOS = make_unique<raw_svector_ostream>(Buffer);
            OS = BOS.get();
        }

        LLVMTargetMachine &LLVMTM = static_cast<LLVMTargetMachine&>(*Target);
        MachineModuleInfo *MMI = new MachineModuleInfo(&LLVMTM);

        // Construct a custom pass pipeline that starts after instruction
        // selection.
        if (!RunPassNames->empty()) {
            if (!MIR) {
                WithColor::warning(errs(), progName)
                    << "run-pass is for .mir file only.\n";
                return 1;
            }
            TargetPassConfig &TPC = *LLVMTM.createPassConfig(PM);
            if (TPC.hasLimitedCodeGenPipeline()) {
                WithColor::warning(errs(), progName)
                    << "run-pass cannot be used with "
                    << TPC.getLimitedCodeGenPipelineReason(" and ") << ".\n";
                return 1;
            }

            TPC.setDisableVerify(NoVerify);
            PM.add(&TPC);
            PM.add(MMI);
            TPC.printAndVerify("");
            for (const std::string &RunPassName : *RunPassNames) {
                if (addPass(PM, progName, RunPassName, TPC))
                    return 1;
            }
            TPC.setInitialized();
            PM.add(createPrintMIRPass(*OS));
            PM.add(createFreeMachineFunctionPass());
        }
        else if (Target->addPassesToEmitFile(PM, *OS,
            DwoOut ? &DwoOut->os() : nullptr,
            FileType, NoVerify, MMI)) {
            WithColor::warning(errs(), progName)
                << "target does not support generation of this"
                << " file type!\n";
            return 1;
        }

        if (MIR) {
            assert(MMI && "Forgot to create MMI?");
            if (MIR->parseMachineFunctions(*M, *MMI))
                return 1;
        }

        // Before executing passes, print the final values of the LLVM options.
        cl::PrintOptionValues();

        // If requested, run the pass manager over the same module again,
        // to catch any bugs due to persistent state in the passes. Note that
        // opt has the same functionality, so it may be worth abstracting this out
        // in the future.
        SmallVector<char, 0> CompileTwiceBuffer;
        if (CompileTwice) {
            std::unique_ptr<Module> M2(llvm::CloneModule(*M));
            PM.run(*M2);
            CompileTwiceBuffer = Buffer;
            Buffer.clear();
        }

        PM.run(*M);

        auto HasError =
            ((const LLCDiagnosticHandler *)(Context.getDiagHandlerPtr()))->HasError;
        if (*HasError)
            return 1;

        // Compare the two outputs and make sure they're the same
        if (CompileTwice) {
            if (Buffer.size() != CompileTwiceBuffer.size() ||
                (memcmp(Buffer.data(), CompileTwiceBuffer.data(), Buffer.size()) !=
                    0)) {
                errs()
                    << "Running the pass manager twice changed the output.\n"
                    "Writing the result of the second run to the specified output\n"
                    "To generate the one-run comparison binary, just run without\n"
                    "the compile-twice option\n";
                Out->os() << Buffer;
                Out->keep();
                return 1;
            }
        }

        if (BOS) {
            Out->os() << Buffer;
        }
    }

    // Declare success.
    Out->keep();
    if (DwoOut)
        DwoOut->keep();

    return 0;
}
