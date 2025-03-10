/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GLSLPostProcessor.h"

#include <sstream>
#include <vector>

#include <GlslangToSpv.h>
#include <SPVRemapper.h>
#include <localintermediate.h>

#include <spirv_glsl.hpp>
#include <spirv_msl.hpp>

#include "sca/builtinResource.h"
#include "sca/GLSLTools.h"

#include <utils/Log.h>

using namespace glslang;
using namespace spirv_cross;
using namespace spvtools;

namespace filamat {

GLSLPostProcessor::GLSLPostProcessor(MaterialBuilder::Optimization optimization, uint32_t flags)
        : mOptimization(optimization),
          mPrintShaders(flags & PRINT_SHADERS),
          mGenerateDebugInfo(flags & GENERATE_DEBUG_INFO) {
}

GLSLPostProcessor::~GLSLPostProcessor() {
}

static uint32_t shaderVersionFromModel(filament::backend::ShaderModel model) {
    switch (model) {
        case filament::backend::ShaderModel::UNKNOWN:
        case filament::backend::ShaderModel::GL_ES_30:
            return 300;
        case filament::backend::ShaderModel::GL_CORE_41:
            return 410;
    }
}

static void errorHandler(const std::string& str) {
    utils::slog.e << str << utils::io::endl;
}

static std::string stringifySpvOptimizerMessage(spv_message_level_t level, const char* source,
        const spv_position_t& position, const char* message) {
    const char* levelString = nullptr;
    switch (level) {
        case SPV_MSG_FATAL:
            levelString = "FATAL";
            break;
        case SPV_MSG_INTERNAL_ERROR:
            levelString = "INTERNAL ERROR";
            break;
        case SPV_MSG_ERROR:
            levelString = "ERROR";
            break;
        case SPV_MSG_WARNING:
            levelString = "WARNING";
            break;
        case SPV_MSG_INFO:
            levelString = "INFO";
            break;
        case SPV_MSG_DEBUG:
            levelString = "DEBUG";
            break;
    }

    std::ostringstream oss;
    oss << levelString << ": ";
    if (source) oss << source << ":";
    oss << position.line << ":" << position.column << ":";
    oss << position.index << ": ";
    if (message) oss << message;

    return oss.str();
}

/**
 * Shrinks the specified string and returns a new string as the result.
 * To shrink the string, this method performs the following transforms:
 * - Remove leading white spaces at the beginning of each line
 * - Remove empty lines
 */
static std::string shrinkString(const std::string& s) {
    size_t cur = 0;

    std::string r;
    r.reserve(s.length());

    while (cur < s.length()) {
        size_t pos = cur;
        size_t len = 0;

        while (s[cur] != '\n') {
            cur++;
            len++;
        }

        size_t newPos = s.find_first_not_of(" \t", pos);
        if (newPos == std::string::npos) newPos = pos;

        r.append(s, newPos, len - (newPos - pos));
        r += '\n';

        while (s[cur] == '\n') {
            cur++;
        }
    }

    return r;
}

void SpvToMsl(const SpirvBlob* spirv, std::string* outMsl, const GLSLPostProcessor::Config& config) {
    CompilerMSL mslCompiler(*spirv);
    CompilerGLSL::Options options;
    options.vertex.fixup_clipspace = true;
    mslCompiler.set_common_options(options);

    const CompilerMSL::Options::Platform platform =
        config.shaderModel == filament::backend::ShaderModel::GL_ES_30 ?
            CompilerMSL::Options::Platform::iOS : CompilerMSL::Options::Platform::macOS;
    mslCompiler.set_msl_options(CompilerMSL::Options {
        .platform = platform,
        .msl_version = CompilerMSL::Options::make_msl_version(1, 1),
        .ios_use_framebuffer_fetch_subpasses = true
    });

    auto executionModel = mslCompiler.get_execution_model();

    auto duplicateResourceBinding = [executionModel, &mslCompiler](const auto& resource) {
        auto set = mslCompiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
        auto binding = mslCompiler.get_decoration(resource.id, spv::DecorationBinding);
        MSLResourceBinding newBinding;
        newBinding.stage = executionModel;
        newBinding.desc_set = set;
        newBinding.binding = binding;
        newBinding.msl_texture = binding;
        newBinding.msl_sampler = binding;
        newBinding.msl_buffer = binding;
        mslCompiler.add_msl_resource_binding(newBinding);
    };

    auto resources = mslCompiler.get_shader_resources();
    for (const auto& resource : resources.sampled_images) {
        duplicateResourceBinding(resource);
    }
    for (const auto& resource : resources.uniform_buffers) {
        duplicateResourceBinding(resource);
    }

    *outMsl = mslCompiler.compile();
    *outMsl = shrinkString(*outMsl);
}

bool GLSLPostProcessor::process(const std::string& inputShader, Config const& config,
        std::string* outputGlsl, SpirvBlob* outputSpirv, std::string* outputMsl) {

    // If TargetApi is Vulkan, then we need post-processing even if there's no optimization.
    using TargetApi = MaterialBuilder::TargetApi;
    const TargetApi targetApi = outputSpirv ? TargetApi::VULKAN : TargetApi::OPENGL;
    if (targetApi == TargetApi::OPENGL && mOptimization == MaterialBuilder::Optimization::NONE) {
        *outputGlsl = inputShader;
        if (mPrintShaders) {
            utils::slog.i << *outputGlsl << utils::io::endl;
        }
        return true;
    }

    mGlslOutput = outputGlsl;
    mSpirvOutput = outputSpirv;
    mMslOutput = outputMsl;

    if (config.shaderType == filament::backend::VERTEX) {
        mShLang = EShLangVertex;
    } else {
        mShLang = EShLangFragment;
    }

    TProgram program;
    TShader tShader(mShLang);

    // The cleaner must be declared after the TShader to prevent ASAN failures.
    GLSLangCleaner cleaner;

    const char* shaderCString = inputShader.c_str();
    tShader.setStrings(&shaderCString, 1);

    mLangVersion = GLSLTools::glslangVersionFromShaderModel(config.shaderModel);
    GLSLTools::prepareShaderParser(tShader, mShLang, mLangVersion, mOptimization);
    EShMessages msg = GLSLTools::glslangFlagsFromTargetApi(targetApi);
    bool ok = tShader.parse(&DefaultTBuiltInResource, mLangVersion, false, msg);
    if (!ok) {
        utils::slog.e << tShader.getInfoLog() << utils::io::endl;
        return false;
    }

    program.addShader(&tShader);
    // Even though we only have a single shader stage, linking is still necessary to finalize
    // SPIR-V types
    bool linkOk = program.link(msg);
    if (!linkOk) {
        utils::slog.e << tShader.getInfoLog() << utils::io::endl;
        return false;
    }

    switch (mOptimization) {
        case MaterialBuilder::Optimization::NONE:
            if (mSpirvOutput) {
                SpvOptions options;
                options.generateDebugInfo = mGenerateDebugInfo;
                GlslangToSpv(*program.getIntermediate(mShLang), *mSpirvOutput, &options);
                if (mMslOutput) {
                    SpvToMsl(mSpirvOutput, mMslOutput, config);
                }
            } else {
                utils::slog.e << "GLSL post-processor invoked with optimization level NONE"
                        << utils::io::endl;
            }
            break;
        case MaterialBuilder::Optimization::PREPROCESSOR:
            preprocessOptimization(tShader, config);
            break;
        case MaterialBuilder::Optimization::SIZE:
        case MaterialBuilder::Optimization::PERFORMANCE:
            fullOptimization(tShader, config);
            break;
    }

    if (mGlslOutput) {
        *mGlslOutput = shrinkString(*mGlslOutput);
        if (mPrintShaders) {
            utils::slog.i << *mGlslOutput << utils::io::endl;
        }
    }
    return true;
}

void GLSLPostProcessor::preprocessOptimization(glslang::TShader& tShader,
        GLSLPostProcessor::Config const& config) const {
    using TargetApi = MaterialBuilder::TargetApi;

    std::string glsl;
    TShader::ForbidIncluder forbidIncluder;

    int version = GLSLTools::glslangVersionFromShaderModel(config.shaderModel);
    const TargetApi targetApi = mSpirvOutput ? TargetApi::VULKAN : TargetApi::OPENGL;
    EShMessages msg = GLSLTools::glslangFlagsFromTargetApi(targetApi);
    bool ok = tShader.preprocess(&DefaultTBuiltInResource, version, ENoProfile, false, false,
            msg, &glsl, forbidIncluder);

    if (!ok) {
        utils::slog.e << tShader.getInfoLog() << utils::io::endl;
    }

    if (mSpirvOutput) {
        TProgram program;
        TShader spirvShader(mShLang);

        // The cleaner must be declared after the TShader/TProgram which are setting the current
        // pool in the tls
        GLSLangCleaner cleaner;

        const char* shaderCString = glsl.c_str();
        spirvShader.setStrings(&shaderCString, 1);
        GLSLTools::prepareShaderParser(spirvShader, mShLang, mLangVersion, mOptimization);
        ok = spirvShader.parse(&DefaultTBuiltInResource, mLangVersion, false, msg);
        program.addShader(&spirvShader);
        // Even though we only have a single shader stage, linking is still necessary to finalize
        // SPIR-V types
        bool linkOk = program.link(msg);
        if (!ok || !linkOk) {
            utils::slog.e << spirvShader.getInfoLog() << utils::io::endl;
        } else {
            SpvOptions options;
            options.generateDebugInfo = mGenerateDebugInfo;
            GlslangToSpv(*program.getIntermediate(mShLang), *mSpirvOutput, &options);
        }
    }

    if (mMslOutput) {
        SpvToMsl(mSpirvOutput, mMslOutput, config);
    }

    if (mGlslOutput) {
        *mGlslOutput = glsl;
    }
}

void GLSLPostProcessor::fullOptimization(const TShader& tShader,
        GLSLPostProcessor::Config const& config) const {
    SpirvBlob spirv;

    // Compile GLSL to to SPIR-V
    SpvOptions options;
    options.generateDebugInfo = mGenerateDebugInfo;
    GlslangToSpv(*tShader.getIntermediate(), spirv, &options);

    // Run the SPIR-V optimizer
    OptimizerPtr optimizer = createOptimizer(mOptimization, config);
    optimizeSpirv(optimizer, spirv);

    if (mSpirvOutput) {
        *mSpirvOutput = spirv;
    }

    if (mMslOutput) {
        SpvToMsl(&spirv, mMslOutput, config);
    }

    // Transpile back to GLSL
    if (mGlslOutput) {
        CompilerGLSL::Options glslOptions;
        glslOptions.es = config.shaderModel == filament::backend::ShaderModel::GL_ES_30;
        glslOptions.version = shaderVersionFromModel(config.shaderModel);
        glslOptions.enable_420pack_extension = glslOptions.version >= 420;
        glslOptions.fragment.default_float_precision = glslOptions.es ?
                CompilerGLSL::Options::Precision::Mediump : CompilerGLSL::Options::Precision::Highp;
        glslOptions.fragment.default_int_precision = glslOptions.es ?
                CompilerGLSL::Options::Precision::Mediump : CompilerGLSL::Options::Precision::Highp;

        CompilerGLSL glslCompiler(move(spirv));
        glslCompiler.set_common_options(glslOptions);

        if (tShader.getStage() == EShLangFragment && glslOptions.es) {
            for (auto i : config.glsl.subpassInputToColorLocation) {
                glslCompiler.remap_ext_framebuffer_fetch(i.first, i.second);
            }
        }

        *mGlslOutput = glslCompiler.compile();
    }
}

std::shared_ptr<spvtools::Optimizer> GLSLPostProcessor::createOptimizer(
        MaterialBuilder::Optimization optimization, Config const& config) {
    auto optimizer = std::make_shared<spvtools::Optimizer>(SPV_ENV_UNIVERSAL_1_0);

    optimizer->SetMessageConsumer([](spv_message_level_t level,
            const char* source, const spv_position_t& position, const char* message) {
        utils::slog.e << stringifySpvOptimizerMessage(level, source, position, message)
                << utils::io::endl;
    });

    if (optimization == MaterialBuilder::Optimization::SIZE) {
        registerSizePasses(*optimizer, config);
    } else if (optimization == MaterialBuilder::Optimization::PERFORMANCE) {
        registerPerformancePasses(*optimizer, config);
    }

    return optimizer;
}

void GLSLPostProcessor::optimizeSpirv(OptimizerPtr optimizer, SpirvBlob& spirv) const {
    if (!optimizer->Run(spirv.data(), spirv.size(), &spirv)) {
        utils::slog.e << "SPIR-V optimizer pass failed" << utils::io::endl;
        return;
    }

    // Remove dead module-level objects: functions, types, vars
    spv::spirvbin_t remapper(0);
    remapper.registerErrorHandler(errorHandler);
    remapper.remap(spirv, spv::spirvbin_base_t::DCE_ALL);
}

void GLSLPostProcessor::registerPerformancePasses(Optimizer& optimizer, Config const& config) {
    optimizer
            .RegisterPass(CreateWrapOpKillPass())
            .RegisterPass(CreateDeadBranchElimPass());

    if (config.shaderModel != filament::backend::ShaderModel::GL_CORE_41) {
        // this triggers a segfault with AMD drivers on MacOS
        optimizer.RegisterPass(CreateMergeReturnPass());
    }

    optimizer
            .RegisterPass(CreateInlineExhaustivePass())
            .RegisterPass(CreateAggressiveDCEPass())
            .RegisterPass(CreatePrivateToLocalPass())
            .RegisterPass(CreateLocalSingleBlockLoadStoreElimPass())
            .RegisterPass(CreateLocalSingleStoreElimPass())
            .RegisterPass(CreateAggressiveDCEPass())
            .RegisterPass(CreateScalarReplacementPass())
            .RegisterPass(CreateLocalAccessChainConvertPass())
            .RegisterPass(CreateLocalSingleBlockLoadStoreElimPass())
            .RegisterPass(CreateLocalSingleStoreElimPass())
            .RegisterPass(CreateAggressiveDCEPass())
            .RegisterPass(CreateLocalMultiStoreElimPass())
            .RegisterPass(CreateAggressiveDCEPass())
            .RegisterPass(CreateCCPPass())
            .RegisterPass(CreateAggressiveDCEPass())
            .RegisterPass(CreateRedundancyEliminationPass())
            .RegisterPass(CreateCombineAccessChainsPass())
            .RegisterPass(CreateSimplificationPass())
            .RegisterPass(CreateVectorDCEPass())
            .RegisterPass(CreateDeadInsertElimPass())
            .RegisterPass(CreateDeadBranchElimPass())
            .RegisterPass(CreateSimplificationPass())
            .RegisterPass(CreateIfConversionPass())
            .RegisterPass(CreateCopyPropagateArraysPass())
            .RegisterPass(CreateReduceLoadSizePass())
            .RegisterPass(CreateAggressiveDCEPass())
            .RegisterPass(CreateBlockMergePass())
            .RegisterPass(CreateRedundancyEliminationPass())
            .RegisterPass(CreateDeadBranchElimPass())
            .RegisterPass(CreateBlockMergePass())
            .RegisterPass(CreateSimplificationPass());
}

void GLSLPostProcessor::registerSizePasses(Optimizer& optimizer, Config const& config) {
    optimizer
            .RegisterPass(CreateWrapOpKillPass())
            .RegisterPass(CreateDeadBranchElimPass());

    if (config.shaderModel != filament::backend::ShaderModel::GL_CORE_41) {
        // this triggers a segfault with AMD drivers on MacOS
        optimizer.RegisterPass(CreateMergeReturnPass());
    }

    optimizer
            .RegisterPass(CreateInlineExhaustivePass())
            .RegisterPass(CreateEliminateDeadFunctionsPass())
            .RegisterPass(CreatePrivateToLocalPass())
            .RegisterPass(CreateScalarReplacementPass(0))
            .RegisterPass(CreateLocalMultiStoreElimPass())
            .RegisterPass(CreateCCPPass())
            .RegisterPass(CreateLoopUnrollPass(true))
            .RegisterPass(CreateDeadBranchElimPass())
            .RegisterPass(CreateSimplificationPass())
            .RegisterPass(CreateScalarReplacementPass(0))
            .RegisterPass(CreateLocalSingleStoreElimPass())
            .RegisterPass(CreateIfConversionPass())
            .RegisterPass(CreateSimplificationPass())
            .RegisterPass(CreateAggressiveDCEPass())
            .RegisterPass(CreateDeadBranchElimPass())
            .RegisterPass(CreateBlockMergePass())
            .RegisterPass(CreateLocalAccessChainConvertPass())
            .RegisterPass(CreateLocalSingleBlockLoadStoreElimPass())
            .RegisterPass(CreateAggressiveDCEPass())
            .RegisterPass(CreateCopyPropagateArraysPass())
            .RegisterPass(CreateVectorDCEPass())
            .RegisterPass(CreateDeadInsertElimPass())
            // this breaks UBO layout
            //.RegisterPass(CreateEliminateDeadMembersPass())
            .RegisterPass(CreateLocalSingleStoreElimPass())
            .RegisterPass(CreateBlockMergePass())
            .RegisterPass(CreateLocalMultiStoreElimPass())
            .RegisterPass(CreateRedundancyEliminationPass())
            .RegisterPass(CreateSimplificationPass())
            .RegisterPass(CreateAggressiveDCEPass())
            .RegisterPass(CreateCFGCleanupPass());
}

} // namespace filamat
