from .common.codegen import CodeGen, VulkanWrapperGenerator, VulkanAPIWrapper
from .common.vulkantypes import \
        VulkanAPI, makeVulkanTypeSimple, iterateVulkanType, DISPATCHABLE_HANDLE_TYPES, NON_DISPATCHABLE_HANDLE_TYPES

from .transform import TransformCodegen, genTransformsForVulkanType

from .wrapperdefs import API_PREFIX_MARSHAL
from .wrapperdefs import API_PREFIX_UNMARSHAL
from .wrapperdefs import VULKAN_STREAM_TYPE

from copy import copy

decoder_snapshot_decl_preamble = """

namespace android {
namespace base {
class BumpPool;
class Stream;
} // namespace base {
} // namespace android {

class VkDecoderSnapshot {
public:
    VkDecoderSnapshot();
    ~VkDecoderSnapshot();

    void save(android::base::Stream* stream);
    void load(android::base::Stream* stream, emugl::GfxApiLogger& gfx_logger,
              emugl::HealthMonitor<>* healthMonitor);
    void createExtraHandlesForNextApi(const uint64_t* created, uint32_t count);
"""

decoder_snapshot_decl_postamble = """
private:
    class Impl;
    std::unique_ptr<Impl> mImpl;

};
"""

decoder_snapshot_impl_preamble ="""

using namespace gfxstream::vk;
using emugl::GfxApiLogger;
using emugl::HealthMonitor;

class VkDecoderSnapshot::Impl {
public:
    Impl() { }

    void save(android::base::Stream* stream) {
        mReconstruction.save(stream);
    }

    void load(android::base::Stream* stream, GfxApiLogger& gfx_logger,
              HealthMonitor<>* healthMonitor) {
        mReconstruction.load(stream, gfx_logger, healthMonitor);
    }

    void createExtraHandlesForNextApi(const uint64_t* created, uint32_t count) {
        mReconstruction.createExtraHandlesForNextApi(created, count);
    }
"""

decoder_snapshot_impl_postamble = """
private:
    android::base::Lock mLock;
    VkReconstruction mReconstruction;
};

VkDecoderSnapshot::VkDecoderSnapshot() :
    mImpl(new VkDecoderSnapshot::Impl()) { }

void VkDecoderSnapshot::save(android::base::Stream* stream) {
    mImpl->save(stream);
}

void VkDecoderSnapshot::load(android::base::Stream* stream, GfxApiLogger& gfx_logger,
                             HealthMonitor<>* healthMonitor) {
    mImpl->load(stream, gfx_logger, healthMonitor);
}

void VkDecoderSnapshot::createExtraHandlesForNextApi(const uint64_t* created, uint32_t count) {
    mImpl->createExtraHandlesForNextApi(created, count);
}

VkDecoderSnapshot::~VkDecoderSnapshot() = default;
"""

AUXILIARY_SNAPSHOT_API_BASE_PARAM_COUNT = 3

AUXILIARY_SNAPSHOT_API_PARAM_NAMES = [
    "input_result",
]

# Vulkan handle dependencies.
# (a, b): a depends on b
SNAPSHOT_HANDLE_DEPENDENCIES = [
    # Dispatchable handle types
    ("VkCommandBuffer", "VkCommandPool"),
    ("VkCommandPool", "VkDevice"),
    ("VkQueue", "VkDevice"),
    ("VkDevice", "VkPhysicalDevice"),
    ("VkPhysicalDevice", "VkInstance")] + \
    list(map(lambda handleType : (handleType, "VkDevice"), NON_DISPATCHABLE_HANDLE_TYPES))

handleDependenciesDict = dict(SNAPSHOT_HANDLE_DEPENDENCIES)

def extract_deps_vkAllocateCommandBuffers(param, access, lenExpr, api, cgen):
    cgen.stmt("mReconstruction.addHandleDependency((const uint64_t*)%s, %s, (uint64_t)(uintptr_t)%s)" % \
              (access, lenExpr, "unboxed_to_boxed_non_dispatchable_VkCommandPool(pAllocateInfo->commandPool)"))

def extract_deps_vkCreateImageView(param, access, lenExpr, api, cgen):
    cgen.stmt("mReconstruction.addHandleDependency((const uint64_t*)%s, %s, (uint64_t)(uintptr_t)%s)" % \
              (access, lenExpr, "unboxed_to_boxed_non_dispatchable_VkImage(pCreateInfo->image)"))

def extract_deps_vkCreateGraphicsPipelines(param, access, lenExpr, api, cgen):
    cgen.beginFor("uint32_t i = 0", "i < createInfoCount", "++i")
    cgen.beginFor("uint32_t j = 0", "j < pCreateInfos[i].stageCount", "++j")
    cgen.stmt("mReconstruction.addHandleDependency((const uint64_t*)(%s + i), %s, (uint64_t)(uintptr_t)%s)" % \
              (access, 1, "unboxed_to_boxed_non_dispatchable_VkShaderModule(pCreateInfos[i].pStages[j].module)"))
    cgen.endFor()
    cgen.endFor()

def extract_deps_vkCreateFramebuffer(param, access, lenExpr, api, cgen):
    cgen.stmt("mReconstruction.addHandleDependency((const uint64_t*)%s, %s, (uint64_t)(uintptr_t)%s)" % \
              (access, lenExpr, "unboxed_to_boxed_non_dispatchable_VkRenderPass(pCreateInfo->renderPass)"))

def extract_deps_vkBindImageMemory(param, access, lenExpr, api, cgen):
    cgen.stmt("mReconstruction.addHandleDependency((const uint64_t*)%s, %s, (uint64_t)(uintptr_t)%s)" % \
              (access, lenExpr, "(uint64_t)(uintptr_t)unboxed_to_boxed_non_dispatchable_VkDeviceMemory(memory)"))

specialCaseDependencyExtractors = {
    "vkAllocateCommandBuffers" : extract_deps_vkAllocateCommandBuffers,
    "vkCreateImageView" : extract_deps_vkCreateImageView,
    "vkCreateGraphicsPipelines" : extract_deps_vkCreateGraphicsPipelines,
    "vkCreateFramebuffer" : extract_deps_vkCreateFramebuffer,
    "vkBindImageMemory": extract_deps_vkBindImageMemory,
}

apiSequences = {
    "vkAllocateMemory" : ["vkAllocateMemory", "vkMapMemoryIntoAddressSpaceGOOGLE"]
}

# vkBindImageMemory needs to execute before vkCreateImageView
# Thus we inject it into VkImage creation sequence.
# TODO: add vkBindImageMemory2 into this list
apiInitializes = {
    "vkBindImageMemory": ["image"],
}

apiModifies = {
    "vkMapMemoryIntoAddressSpaceGOOGLE" : ["memory"],
    "vkGetBlobGOOGLE" : ["memory"],
}

def is_initialize_operation(api, param):
    if api.name in apiInitializes:
        if param.paramName in apiInitializes[api.name]:
            return True
    return False

def is_modify_operation(api, param):
    if api.name in apiModifies:
        if param.paramName in apiModifies[api.name]:
            return True
    return False

def emit_impl(typeInfo, api, cgen):
    for p in api.parameters:
        if not (p.isHandleType):
            continue

        lenExpr = cgen.generalLengthAccess(p)
        lenAccessGuard = cgen.generalLengthAccessGuard(p)

        if lenExpr is None:
            lenExpr = "1"

        # Note that in vkCreate*, the last parameter (the output) is boxed. But all input parameters are unboxed.

        if p.pointerIndirectionLevels > 0:
            access = p.paramName
        else:
            access = "(&%s)" % p.paramName

        if p.isCreatedBy(api) or is_initialize_operation(api, p):
            if p.isCreatedBy(api):
                boxed_access = access
            else:
                cgen.stmt("%s boxed_%s = unboxed_to_boxed_non_dispatchable_%s(%s[0])" % (p.typeName, p.typeName, p.typeName, access))
                boxed_access = "&boxed_%s" % p.typeName
            if p.pointerIndirectionLevels > 0:
                cgen.stmt("if (!%s) return" % access)
            cgen.stmt("android::base::AutoLock lock(mLock)")
            cgen.line("// %s create" % p.paramName)
            if p.isCreatedBy(api):
                cgen.stmt("mReconstruction.addHandles((const uint64_t*)%s, %s)" % (boxed_access, lenExpr));

            if p.isCreatedBy(api) and p.typeName in handleDependenciesDict:
                dependsOnType = handleDependenciesDict[p.typeName];
                for p2 in api.parameters:
                    if p2.typeName == dependsOnType:
                        cgen.stmt("mReconstruction.addHandleDependency((const uint64_t*)%s, %s, (uint64_t)(uintptr_t)%s)" % (boxed_access, lenExpr, p2.paramName))
            if api.name in specialCaseDependencyExtractors:
                specialCaseDependencyExtractors[api.name](p, boxed_access, lenExpr, api, cgen)

            cgen.stmt("auto apiHandle = mReconstruction.createApiInfo()")
            cgen.stmt("auto apiInfo = mReconstruction.getApiInfo(apiHandle)")
            cgen.stmt("mReconstruction.setApiTrace(apiInfo, OP_%s, snapshotTraceBegin, snapshotTraceBytes)" % api.name)
            if lenAccessGuard is not None:
                cgen.beginIf(lenAccessGuard)
            cgen.stmt("mReconstruction.forEachHandleAddApi((const uint64_t*)%s, %s, apiHandle)" % (boxed_access, lenExpr))
            if p.isCreatedBy(api):
                cgen.stmt("mReconstruction.setCreatedHandlesForApi(apiHandle, (const uint64_t*)%s, %s)" % (boxed_access, lenExpr))
            if lenAccessGuard is not None:
                cgen.endIf()

        if p.isDestroyedBy(api):
            cgen.stmt("android::base::AutoLock lock(mLock)")
            cgen.line("// %s destroy" % p.paramName)
            if lenAccessGuard is not None:
                cgen.beginIf(lenAccessGuard)
            cgen.stmt("mReconstruction.removeHandles((const uint64_t*)%s, %s)" % (access, lenExpr));
            if lenAccessGuard is not None:
                cgen.endIf()

        if is_modify_operation(api, p):
            cgen.stmt("android::base::AutoLock lock(mLock)")
            cgen.line("// %s modify" % p.paramName)
            cgen.stmt("auto apiHandle = mReconstruction.createApiInfo()")
            cgen.stmt("auto apiInfo = mReconstruction.getApiInfo(apiHandle)")
            cgen.stmt("mReconstruction.setApiTrace(apiInfo, OP_%s, snapshotTraceBegin, snapshotTraceBytes)" % api.name)
            if lenAccessGuard is not None:
                cgen.beginIf(lenAccessGuard)
            cgen.beginFor("uint32_t i = 0", "i < %s" % lenExpr, "++i")
            if p.isNonDispatchableHandleType():
                cgen.stmt("%s boxed = unboxed_to_boxed_non_dispatchable_%s(%s[i])" % (p.typeName, p.typeName, access))
            else:
                cgen.stmt("%s boxed = unboxed_to_boxed_%s(%s[i])" % (p.typeName, p.typeName, access))
            cgen.stmt("mReconstruction.forEachHandleAddModifyApi((const uint64_t*)(&boxed), 1, apiHandle)")
            cgen.endFor()
            if lenAccessGuard is not None:
                cgen.endIf()

def emit_passthrough_to_impl(typeInfo, api, cgen):
    cgen.vkApiCall(api, customPrefix = "mImpl->")

class VulkanDecoderSnapshot(VulkanWrapperGenerator):
    def __init__(self, module, typeInfo):
        VulkanWrapperGenerator.__init__(self, module, typeInfo)

        self.typeInfo = typeInfo

        self.cgenHeader = CodeGen()
        self.cgenHeader.incrIndent()

        self.cgenImpl = CodeGen()

        self.currentFeature = None

        self.feature_apis = []

    def onBegin(self,):
        self.module.appendHeader(decoder_snapshot_decl_preamble)
        self.module.appendImpl(decoder_snapshot_impl_preamble)

    def onBeginFeature(self, featureName, featureType):
        VulkanWrapperGenerator.onBeginFeature(self, featureName, featureType)
        self.currentFeature = featureName

    def onGenCmd(self, cmdinfo, name, alias):
        VulkanWrapperGenerator.onGenCmd(self, cmdinfo, name, alias)

        api = self.typeInfo.apis[name]

        additionalParams = [ \
            makeVulkanTypeSimple(True, "uint8_t", 1, "snapshotTraceBegin"),
            makeVulkanTypeSimple(False, "size_t", 0, "snapshotTraceBytes"),
            makeVulkanTypeSimple(False, "android::base::BumpPool", 1, "pool"),]

        if api.retType.typeName != "void":
            additionalParams.append( \
                makeVulkanTypeSimple(False, api.retType.typeName, 0, "input_result"))

        apiForSnapshot = \
            api.withCustomParameters( \
                additionalParams + \
                api.parameters).withCustomReturnType( \
                    makeVulkanTypeSimple(False, "void", 0, "void"))

        self.feature_apis.append((self.currentFeature, apiForSnapshot))

        self.cgenHeader.stmt(self.cgenHeader.makeFuncProto(apiForSnapshot))
        self.module.appendHeader(self.cgenHeader.swapCode())

        self.cgenImpl.emitFuncImpl( \
            apiForSnapshot, lambda cgen: emit_impl(self.typeInfo, apiForSnapshot, cgen))
        self.module.appendImpl(self.cgenImpl.swapCode())

    def onEnd(self,):
        self.module.appendHeader(decoder_snapshot_decl_postamble)
        self.module.appendImpl(decoder_snapshot_impl_postamble)
        self.cgenHeader.decrIndent()

        for feature, api in self.feature_apis:
            if feature is not None:
                self.cgenImpl.line("#ifdef %s" % feature)

            apiImplShell = \
                api.withModifiedName("VkDecoderSnapshot::" + api.name)

            self.cgenImpl.emitFuncImpl( \
                apiImplShell, lambda cgen: emit_passthrough_to_impl(self.typeInfo, api, cgen))

            if feature is not None:
                self.cgenImpl.line("#endif")

        self.module.appendImpl(self.cgenImpl.swapCode())

