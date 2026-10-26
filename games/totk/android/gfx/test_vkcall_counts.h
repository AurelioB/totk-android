/* Test-only driver API counts. Install before submitting work; atomics include all recorder threads. */
static const char *call_counts_path;
static _Atomic uint64_t call_buffers, call_sets;
#define CALL_R(name, params, args, extra) \
    static PFN_##name real_##name; static _Atomic uint64_t count_##name; \
    static VKAPI_ATTR VkResult VKAPI_CALL counted_##name params { \
        atomic_fetch_add(&count_##name, 1); extra; return real_##name args; }
#define CALL_V(name, params, args) \
    static PFN_##name real_##name; static _Atomic uint64_t count_##name; \
    static VKAPI_ATTR void VKAPI_CALL counted_##name params { \
        atomic_fetch_add(&count_##name, 1); real_##name args; }
#define RESULT_CALLS(X) \
    X(vkCreateCommandPool, (VkDevice d, const VkCommandPoolCreateInfo *i, const VkAllocationCallbacks *a, VkCommandPool *p), (d,i,a,p), (void)0) \
    X(vkResetCommandPool, (VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f), (d,p,f), (void)0) \
    X(vkAllocateCommandBuffers, (VkDevice d, const VkCommandBufferAllocateInfo *i, VkCommandBuffer *c), (d,i,c), atomic_fetch_add(&call_buffers,i->commandBufferCount)) \
    X(vkBeginCommandBuffer, (VkCommandBuffer c, const VkCommandBufferBeginInfo *i), (c,i), (void)0) \
    X(vkEndCommandBuffer, (VkCommandBuffer c), (c), (void)0) \
    X(vkResetDescriptorPool, (VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f), (d,p,f), (void)0) \
    X(vkAllocateDescriptorSets, (VkDevice d, const VkDescriptorSetAllocateInfo *i, VkDescriptorSet *s), (d,i,s), atomic_fetch_add(&call_sets,i->descriptorSetCount)) \
    X(vkCreateDescriptorUpdateTemplate, (VkDevice d, const VkDescriptorUpdateTemplateCreateInfo *i, const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *t), (d,i,a,t), (void)0)
#define VOID_CALLS(X) \
    X(vkUpdateDescriptorSets, (VkDevice d, uint32_t n, const VkWriteDescriptorSet *w, uint32_t m, const VkCopyDescriptorSet *c), (d,n,w,m,c)) \
    X(vkUpdateDescriptorSetWithTemplate, (VkDevice d, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void *p), (d,s,t,p)) \
    X(vkCmdBindPipeline, (VkCommandBuffer c, VkPipelineBindPoint b, VkPipeline p), (c,b,p)) \
    X(vkCmdBindVertexBuffers2, (VkCommandBuffer c, uint32_t f, uint32_t n, const VkBuffer *b, const VkDeviceSize *o, const VkDeviceSize *s, const VkDeviceSize *t), (c,f,n,b,o,s,t)) \
    X(vkCmdBindIndexBuffer, (VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkIndexType t), (c,b,o,t)) \
    X(vkCmdBindDescriptorSets, (VkCommandBuffer c, VkPipelineBindPoint b, VkPipelineLayout l, uint32_t f, uint32_t n, const VkDescriptorSet *s, uint32_t m, const uint32_t *o), (c,b,l,f,n,s,m,o)) \
    X(vkCmdExecuteCommands, (VkCommandBuffer c, uint32_t n, const VkCommandBuffer *s), (c,n,s)) \
    X(vkCmdSetViewport, (VkCommandBuffer c, uint32_t f, uint32_t n, const VkViewport *v), (c,f,n,v)) \
    X(vkCmdSetScissor, (VkCommandBuffer c, uint32_t f, uint32_t n, const VkRect2D *r), (c,f,n,r)) \
    X(vkCmdSetDepthBias, (VkCommandBuffer c, float a, float b, float d), (c,a,b,d)) \
    X(vkCmdSetBlendConstants, (VkCommandBuffer c, const float b[4]), (c,b)) \
    X(vkCmdSetStencilCompareMask, (VkCommandBuffer c, VkStencilFaceFlags f, uint32_t m), (c,f,m)) \
    X(vkCmdSetStencilWriteMask, (VkCommandBuffer c, VkStencilFaceFlags f, uint32_t m), (c,f,m)) \
    X(vkCmdSetStencilReference, (VkCommandBuffer c, VkStencilFaceFlags f, uint32_t m), (c,f,m)) \
    X(vkCmdSetCullMode, (VkCommandBuffer c, VkCullModeFlags f), (c,f)) \
    X(vkCmdSetFrontFace, (VkCommandBuffer c, VkFrontFace f), (c,f)) \
    X(vkCmdSetPrimitiveTopology, (VkCommandBuffer c, VkPrimitiveTopology t), (c,t)) \
    X(vkCmdSetDepthTestEnable, (VkCommandBuffer c, VkBool32 b), (c,b)) \
    X(vkCmdSetDepthWriteEnable, (VkCommandBuffer c, VkBool32 b), (c,b)) \
    X(vkCmdSetDepthCompareOp, (VkCommandBuffer c, VkCompareOp o), (c,o)) \
    X(vkCmdSetStencilTestEnable, (VkCommandBuffer c, VkBool32 b), (c,b)) \
    X(vkCmdSetStencilOp, (VkCommandBuffer c, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b, VkStencilOp d, VkCompareOp o), (c,f,a,b,d,o)) \
    X(vkCmdSetDepthBiasEnable, (VkCommandBuffer c, VkBool32 b), (c,b)) \
    X(vkCmdSetPrimitiveRestartEnable, (VkCommandBuffer c, VkBool32 b), (c,b)) \
    X(vkCmdSetLineWidth, (VkCommandBuffer c, float w), (c,w))
RESULT_CALLS(CALL_R)
VOID_CALLS(CALL_V)
#undef CALL_R
#undef CALL_V
static void call_counts_dump(void) {
    FILE *f = fopen(call_counts_path, "w"); if (!f) DIE("cannot write API counts: %s", call_counts_path);
    fprintf(f, "{\n");
#define PRINT_CALL(name, ...) fprintf(f, "  \"%s\": %llu,\n", #name, (unsigned long long)atomic_load(&count_##name));
    RESULT_CALLS(PRINT_CALL) VOID_CALLS(PRINT_CALL)
#undef PRINT_CALL
    fprintf(f, "  \"allocated_command_buffers\": %llu,\n  \"allocated_descriptor_sets\": %llu\n}\n",
        (unsigned long long)atomic_load(&call_buffers), (unsigned long long)atomic_load(&call_sets));
    fclose(f);
}
static void call_counts_install(void) {
    call_counts_path = getenv("TOTK_VK_TEST_CALL_COUNTS"); if (!call_counts_path) return;
#define INSTALL_CALL(name, ...) real_##name = name; name = counted_##name;
    RESULT_CALLS(INSTALL_CALL) VOID_CALLS(INSTALL_CALL)
#undef INSTALL_CALL
    atexit(call_counts_dump);
}
#undef RESULT_CALLS
#undef VOID_CALLS
