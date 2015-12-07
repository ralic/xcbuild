/**
 Copyright (c) 2015-present, Facebook, Inc.
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree. An additional grant
 of patent rights can be found in the PATENTS file in the same directory.
 */

#include <pbxbuild/Tool/ClangResolver.h>
#include <pbxbuild/Tool/HeadermapResolver.h>
#include <pbxbuild/Tool/CompilationInfo.h>
#include <pbxbuild/Tool/HeadermapInfo.h>
#include <pbxbuild/Tool/PrecompiledHeaderInfo.h>
#include <pbxbuild/Tool/SearchPaths.h>
#include <pbxbuild/Tool/ToolEnvironment.h>
#include <pbxbuild/Tool/OptionsResult.h>
#include <pbxbuild/Tool/CommandLineResult.h>
#include <pbxbuild/TypeResolvedFile.h>

using pbxbuild::Tool::ClangResolver;
using pbxbuild::Tool::ToolEnvironment;
using pbxbuild::Tool::OptionsResult;
using pbxbuild::Tool::CommandLineResult;
using pbxbuild::Tool::CompilationInfo;
using pbxbuild::Tool::HeadermapInfo;
using pbxbuild::Tool::PrecompiledHeaderInfo;
using pbxbuild::Tool::SearchPaths;
using pbxbuild::ToolInvocation;
using pbxbuild::TypeResolvedFile;
using libutil::FSUtil;

ClangResolver::
ClangResolver(pbxspec::PBX::Compiler::shared_ptr const &compiler) :
    _compiler(compiler)
{
}

ClangResolver::
~ClangResolver()
{
}

static bool
DialectIsCPlusPlus(std::string const &dialect)
{
    return (dialect.size() > 2 && dialect.substr(dialect.size() - 2) == "++");
}

static void
AppendDialectFlags(std::vector<std::string> *args, std::string const &dialect, std::string const &dialectSuffix)
{
    args->push_back("-x");
    args->push_back(dialect + dialectSuffix);
}

static void
AppendCompoundFlag(std::vector<std::string> *args, std::string const &path, std::string const &prefix, bool concatenate)
{
    if (concatenate) {
        args->push_back(prefix + path);
    } else {
        args->push_back(prefix);
        args->push_back(path);
    }
}

static void
AppendCompoundFlags(std::vector<std::string> *args, std::vector<std::string> const &paths, std::string const &prefix, bool concatenate)
{
    for (std::string const &path : paths) {
        AppendCompoundFlag(args, path, prefix, concatenate);
    }
}

static void
AppendPathFlags(std::vector<std::string> *args, pbxsetting::Environment const &environment, SearchPaths const &searchPaths, HeadermapInfo const &headermapInfo)
{
    AppendCompoundFlags(args, headermapInfo.systemHeadermapFiles(), "-I", true);
    AppendCompoundFlags(args, headermapInfo.userHeadermapFiles(), "-iquote", false);

    if (environment.resolve("USE_HEADER_SYMLINKS") == "YES") {
        // TODO(grp): Create this symlink tree as needed.
        AppendCompoundFlags(args, { environment.resolve("CPP_HEADER_SYMLINKS_DIR") }, "-I", true);
    }

    std::vector<std::string> specialIncludePaths = {
        environment.resolve("DERIVED_FILE_DIR"),
        environment.resolve("DERIVED_FILE_DIR") + "/" + environment.resolve("arch"),
        environment.resolve("BUILT_PRODUCTS_DIR") + "/include",
    };
    AppendCompoundFlags(args, specialIncludePaths, "-I", true);
    AppendCompoundFlags(args, searchPaths.userHeaderSearchPaths(), "-I", true);
    AppendCompoundFlags(args, searchPaths.headerSearchPaths(), "-I", true);

    std::vector<std::string> specialFrameworkPaths = {
        environment.resolve("BUILT_PRODUCTS_DIR"),
    };
    AppendCompoundFlags(args, specialFrameworkPaths, "-F", true);
    AppendCompoundFlags(args, searchPaths.frameworkSearchPaths(), "-F", true);
}

static void
AppendPrefixHeaderFlags(std::vector<std::string> *args, std::string const &prefixHeader)
{
    args->push_back("-include");
    args->push_back(prefixHeader);
}

static void
AppendCustomFlags(std::vector<std::string> *args, pbxsetting::Environment const &environment, std::string const &dialect)
{
    std::vector<std::string> flagSettings;
    if (DialectIsCPlusPlus(dialect)) {
        flagSettings.push_back("OTHER_CPLUSPLUSFLAGS");
    } else {
        flagSettings.push_back("OTHER_CFLAGS");
    }
    flagSettings.push_back("OTHER_CFLAGS_" + environment.resolve("CURRENT_VARIANT"));
    flagSettings.push_back("PER_ARCH_CFLAGS_" + environment.resolve("CURRENT_ARCH"));
    flagSettings.push_back("WARNING_CFLAGS");
    flagSettings.push_back("OPTIMIZATION_CFLAGS");

    for (std::string const &flagSetting : flagSettings) {
        std::vector<std::string> flags = pbxsetting::Type::ParseList(environment.resolve(flagSetting));
        args->insert(args->end(), flags.begin(), flags.end());
    }
}

static void
AppendNotUsedInPrecompsFlags(std::vector<std::string> *args, pbxsetting::Environment const &environment)
{
    std::vector<std::string> preprocessorDefinitions = pbxsetting::Type::ParseList(environment.resolve("GCC_PREPROCESSOR_DEFINITIONS_NOT_USED_IN_PRECOMPS"));
    AppendCompoundFlags(args, preprocessorDefinitions, "-D", true);

    std::vector<std::string> otherFlags = pbxsetting::Type::ParseList(environment.resolve("GCC_OTHER_CFLAGS_NOT_USED_IN_PRECOMPS"));
    args->insert(args->end(), otherFlags.begin(), otherFlags.end());
}

static void
AppendDependencyInfoFlags(std::vector<std::string> *args, pbxspec::PBX::Compiler::shared_ptr const &compiler, pbxsetting::Environment const &environment)
{
    for (pbxsetting::Value const &arg : compiler->dependencyInfoArgs()) {
        args->push_back(environment.expand(arg));
    }
}

static void
AppendInputOutputFlags(std::vector<std::string> *args, pbxspec::PBX::Compiler::shared_ptr const &compiler, std::string const &input, std::string const &output)
{
    std::string sourceFileOption = compiler->sourceFileOption();
    if (sourceFileOption.empty()) {
        sourceFileOption = "-c";
    }
    args->push_back(sourceFileOption);
    args->push_back(input);

    args->push_back("-o");
    args->push_back(output);
}

static std::string
CompileLogMessage(
    pbxspec::PBX::Compiler::shared_ptr const &compiler,
    std::string const &logTitle,
    std::string const &input,
    pbxspec::PBX::FileType::shared_ptr const &fileType,
    std::string const &output,
    pbxsetting::Environment const &environment,
    std::string const &workingDirectory
)
{
    std::string logMessage;
    logMessage += logTitle + " ";
    logMessage += output + " ";
    logMessage += FSUtil::GetRelativePath(input, workingDirectory) + " ";
    logMessage += environment.resolve("variant") + " ";
    logMessage += environment.resolve("arch") + " ";
    logMessage += fileType->GCCDialectName() + " ";
    logMessage += compiler->identifier();
    return logMessage;
}

ToolInvocation ClangResolver::
precompiledHeaderInvocation(
    PrecompiledHeaderInfo const &precompiledHeaderInfo,
    pbxsetting::Environment const &environment,
    std::string const &workingDirectory
) const
{
    std::string const &input = precompiledHeaderInfo.prefixHeader();
    pbxspec::PBX::FileType::shared_ptr const &fileType = precompiledHeaderInfo.fileType();
    std::string output = environment.expand(precompiledHeaderInfo.compileOutputPath());

    pbxspec::PBX::Tool::shared_ptr tool = std::static_pointer_cast <pbxspec::PBX::Tool> (_compiler);
    ToolEnvironment toolEnvironment = ToolEnvironment::Create(tool, environment, { input }, { output });
    pbxsetting::Environment const &env = toolEnvironment.toolEnvironment();
    OptionsResult options = OptionsResult::Create(toolEnvironment, workingDirectory, fileType);
    CommandLineResult commandLine = CommandLineResult::Create(toolEnvironment, options);

    std::vector<std::string> arguments = precompiledHeaderInfo.arguments();
    AppendDependencyInfoFlags(&arguments, _compiler, env);
    AppendInputOutputFlags(&arguments, _compiler, input, output);

    std::string const &dialect = fileType->GCCDialectName();
    std::string logTitle = DialectIsCPlusPlus(dialect) ? "ProcessPCH++" : "ProcessPCH";
    std::string logMessage = CompileLogMessage(_compiler, logTitle, input, fileType, output, env, workingDirectory);

    ToolInvocation::AuxiliaryFile serializedFile = ToolInvocation::AuxiliaryFile(
        env.expand(precompiledHeaderInfo.serializedOutputPath()),
        precompiledHeaderInfo.serialize(),
        false
    );

    return pbxbuild::ToolInvocation(
        commandLine.executable(),
        arguments,
        options.environment(),
        workingDirectory,
        { input },
        { output },
        env.expand(_compiler->dependencyInfoFile()),
        { serializedFile },
        logMessage
    );
}

ToolInvocation ClangResolver::
sourceInvocation(
    TypeResolvedFile const &inputFile,
    std::vector<std::string> const &inputArguments,
    std::string const &outputBaseName,
    HeadermapInfo const &headermapInfo,
    SearchPaths const &searchPaths,
    CompilationInfo *compilationInfo,
    pbxsetting::Environment const &environment,
    std::string const &workingDirectory
) const
{
    std::string outputDirectory = environment.expand(_compiler->outputDir());
    if (outputDirectory.empty()) {
        outputDirectory = environment.expand(pbxsetting::Value::Parse("$(OBJECT_FILE_DIR_$(variant))/$(arch)"));
    }

    std::string outputExtension = _compiler->outputFileExtension();
    if (outputExtension.empty()) {
        outputExtension = "o";
    }

    std::string output = outputDirectory + "/" + outputBaseName + "." + outputExtension;

    std::string const &input = inputFile.filePath();
    pbxspec::PBX::FileType::shared_ptr const &fileType = inputFile.fileType();

    pbxspec::PBX::Tool::shared_ptr tool = std::static_pointer_cast <pbxspec::PBX::Tool> (_compiler);
    ToolEnvironment toolEnvironment = ToolEnvironment::Create(tool, environment, { input }, { output });
    pbxsetting::Environment const &env = toolEnvironment.toolEnvironment();

    OptionsResult options = OptionsResult::Create(toolEnvironment, workingDirectory, fileType);
    CommandLineResult commandLine = CommandLineResult::Create(toolEnvironment, options);

    std::vector<std::string> inputFiles;
    inputFiles.push_back(input);
    inputFiles.insert(inputFiles.end(), headermapInfo.systemHeadermapFiles().begin(), headermapInfo.systemHeadermapFiles().end());
    inputFiles.insert(inputFiles.end(), headermapInfo.userHeadermapFiles().begin(), headermapInfo.userHeadermapFiles().end());

    std::vector<std::string> arguments;
    AppendDialectFlags(&arguments, fileType->GCCDialectName(), "");
    size_t dialectOffset = arguments.size();

    arguments.insert(arguments.end(), commandLine.arguments().begin(), commandLine.arguments().end());
    AppendCustomFlags(&arguments, env, fileType->GCCDialectName());
    AppendPathFlags(&arguments, env, searchPaths, headermapInfo);

    bool precompilePrefixHeader = pbxsetting::Type::ParseBoolean(env.resolve("GCC_PRECOMPILE_PREFIX_HEADER"));
    std::string prefixHeader = env.resolve("GCC_PREFIX_HEADER");
    std::shared_ptr<PrecompiledHeaderInfo> precompiledHeaderInfo = nullptr;

    if (!prefixHeader.empty()) {
        std::string prefixHeaderFile = workingDirectory + "/" + prefixHeader;

        if (precompilePrefixHeader) {
            std::vector<std::string> precompiledHeaderArguments;
            AppendDialectFlags(&precompiledHeaderArguments, fileType->GCCDialectName(), "-header");
            precompiledHeaderArguments.insert(precompiledHeaderArguments.end(), arguments.begin() + dialectOffset, arguments.end());

            precompiledHeaderInfo = std::make_shared<PrecompiledHeaderInfo>(PrecompiledHeaderInfo::Create(_compiler, prefixHeaderFile, fileType, precompiledHeaderArguments));
            AppendPrefixHeaderFlags(&arguments, env.expand(precompiledHeaderInfo->logicalOutputPath()));
            inputFiles.push_back(env.expand(precompiledHeaderInfo->compileOutputPath()));
        } else {
            AppendPrefixHeaderFlags(&arguments, prefixHeaderFile);
            inputFiles.push_back(prefixHeaderFile);
        }
    }

    AppendNotUsedInPrecompsFlags(&arguments, env);
    // After all of the configurable settings, so they can override.
    arguments.insert(arguments.end(), inputArguments.begin(), inputArguments.end());
    AppendDependencyInfoFlags(&arguments, _compiler, env);
    AppendInputOutputFlags(&arguments, _compiler, input, output);

    std::string logMessage = CompileLogMessage(_compiler, "CompileC", input, fileType, output, env, workingDirectory);

    compilationInfo->precompiledHeaderInfo() = precompiledHeaderInfo;

    if (DialectIsCPlusPlus(fileType->GCCDialectName())) {
        /* If a single C++ file is seen, use the C++ linker driver. */
        compilationInfo->linkerDriver() = _compiler->execCPlusPlusLinkerPath();
    } else if (compilationInfo->linkerDriver().empty()) {
        /* If a C file is seen after a C++ file, don't reset back to the C driver. */
        compilationInfo->linkerDriver() = _compiler->execPath();
    }

    compilationInfo->linkerArguments().insert(options.linkerArgs().begin(), options.linkerArgs().end());

    return pbxbuild::ToolInvocation(
        commandLine.executable(),
        arguments,
        options.environment(),
        workingDirectory,
        inputFiles,
        toolEnvironment.outputs(),
        env.expand(_compiler->dependencyInfoFile()),
        { },
        logMessage
    );
}

std::unique_ptr<ClangResolver> ClangResolver::
Create(Phase::PhaseEnvironment const &phaseEnvironment)
{
    pbxbuild::BuildEnvironment const &buildEnvironment = phaseEnvironment.buildEnvironment();
    pbxbuild::TargetEnvironment const &targetEnvironment = phaseEnvironment.targetEnvironment();

    // TODO(grp): This should probably try a number of other compilers if it's not clang.
    std::string gccVersion = targetEnvironment.environment().resolve("GCC_VERSION");

    // TODO(grp): Depending on the build action, add a different suffix than ".compiler".
    pbxspec::PBX::Compiler::shared_ptr defaultCompiler = buildEnvironment.specManager()->compiler(gccVersion + ".compiler", targetEnvironment.specDomains());
    if (defaultCompiler == nullptr) {
        fprintf(stderr, "error: couldn't get default compiler\n");
        return nullptr;
    }

    return std::unique_ptr<ClangResolver>(new ClangResolver(defaultCompiler));
}
