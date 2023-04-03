#include <string>
#include <sstream>
#include <optional>
#include <random>
#include <fstream>

#include <jni.h>

#include <unistd.h>

#include <android/log.h>

#include <sys/mman.h>

#include "include/jvmti.h"

#include "jvmti_allocator.h"

#include "slicer/dex_ir_builder.h"
#include "slicer/reader.h"
#include "slicer/writer.h"
#include "slicer/code_ir.h"
#include "slicer/instrumentation.h"
#include "slicer/control_flow_graph.h"

#include <cmrc/cmrc.hpp>

// Declare the embedded files used to carry along the dex.
CMRC_DECLARE(dex_resources);

#define COVERAGE_MAP_SIZE (64 * 1024)

#define LOG_TAG "ammaraskar"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" JNIEXPORT jstring JNICALL
Java_com_ammaraskar_tool_test_MainActivity_stringFromJNI(
        JNIEnv *env,
        jobject /* this */) {
    ALOGI("I am inside the JNI!");

    pid_t pid = getpid();
    std::string hello = "Hello from C++, my PID is " + std::to_string(pid);
    return env->NewStringUTF(hello.c_str());
}

using namespace lir;

class Transformer {
public:
    explicit Transformer(std::shared_ptr<ir::DexFile> dexIr) : dexIr_(dexIr), builder(dexIr) {
        // Initialize the types and methods we need.
        auto integerType = builder.GetType("I");
        auto instrClassType = builder.GetType("Lcom/ammaraskar/coverageagent/Instrumentation;");
        // proto for `void f(int)`, i.e void function taking an integer arg.
        auto methodProto = builder.GetProto(builder.GetType("V"),
                                            builder.GetTypeList({integerType}));
        instrumentationMethod = builder.GetMethodDecl(builder.GetAsciiString("reachedBlock"), methodProto,
                                                      instrClassType);
    }

    bool transform() {
        for (auto &method : dexIr_->encoded_methods) {
            ALOGI("checking method: %s.%s%s\n",
                   method->decl->parent->Decl().c_str(),
                   method->decl->name->c_str(),
                   method->decl->prototype->Signature().c_str());

            // Do not look into abstract/bridge/native/synthetic methods.
            if ((method->access_flags &
                 (dex::kAccAbstract | dex::kAccBridge | dex::kAccNative | dex::kAccSynthetic)) !=
                0) {
                continue;
            }

            lir::CodeIr codeIr(method.get(), dexIr_);
            lir::ControlFlowGraph cfg(&codeIr, false);

            slicer::AllocateScratchRegs alloc_regs(1);

            alloc_regs.Apply(&codeIr);
            dex::u4 scratch_reg = *alloc_regs.ScratchRegs().begin();

            // TODO: handle very "high" registers
            if (scratch_reg > 0xff) {
                printf("WARNING: can't instrument method %s.%s%s\n",
                       method->decl->parent->Decl().c_str(),
                       method->decl->name->c_str(),
                       method->decl->prototype->Signature().c_str());
                continue;
            }

            // Seed a random number generator with the name of the current method.
            std::string functionName = method->decl->parent->Decl() + "." + method->decl->name->c_str() + "." + method->decl->prototype->Signature();
            std::size_t methodNameHash = std::hash<std::string>{}(functionName);
            std::mt19937 randomGen(methodNameHash);
            std::uniform_int_distribution<> randomDistribution(0, COVERAGE_MAP_SIZE - 1);


            // instrument each basic block entry point
            for (const auto &block : cfg.basic_blocks) {
                // find first bytecode in the basic block
                lir::Instruction *trace_point = nullptr;
                for (auto instr = block.region.first; instr != nullptr; instr = instr->next) {
                    trace_point = dynamic_cast<lir::Bytecode *>(instr);
                    if (trace_point != nullptr || instr == block.region.last) {
                        break;
                    }
                }

                SLICER_CHECK_NE(trace_point, nullptr);
                // special case: don't separate 'move-result-<kind>' from the preceding invoke
                auto opcode = dynamic_cast<lir::Bytecode *>(trace_point)->opcode;
                if (opcode == dex::OP_MOVE_RESULT ||
                    opcode == dex::OP_MOVE_RESULT_WIDE ||
                    opcode == dex::OP_MOVE_RESULT_OBJECT) {
                    trace_point = trace_point->next;
                }

                // Generate a block_id
                int basic_block_id = randomDistribution(randomGen);

                // scratch_reg = block_id
                auto load_block_id = codeIr.Alloc<lir::Bytecode>();
                load_block_id->opcode = dex::OP_CONST;
                load_block_id->operands.push_back(codeIr.Alloc<lir::VReg>(scratch_reg));
                load_block_id->operands.push_back(codeIr.Alloc<lir::Const32>(basic_block_id));
                codeIr.instructions.InsertBefore(trace_point, load_block_id);

                // call Instrumentation.reachedBlock(block_id)
                auto trace_call = codeIr.Alloc<lir::Bytecode>();
                trace_call->opcode = dex::OP_INVOKE_STATIC_RANGE;
                trace_call->operands.push_back(codeIr.Alloc<lir::VRegRange>(scratch_reg, 1));
                trace_call->operands.push_back(codeIr.Alloc<Method>(instrumentationMethod,
                                                                    instrumentationMethod->orig_index));
                codeIr.instructions.InsertBefore(trace_point, trace_call);
            }

            codeIr.Assemble();
        }

        return true;
    }

private:
    std::shared_ptr<ir::DexFile> dexIr_;
    ir::Builder builder;
    ir::MethodDecl *instrumentationMethod = nullptr;
};


// Converts a class name to a type descriptor
// (ex. "java.lang.String" to "Ljava/lang/String;")
std::string classNameToDescriptor(const char *className) {
    std::stringstream ss;
    ss << "L";
    for (auto p = className; *p != '\0'; ++p) {
        ss << (*p == '.' ? '/' : *p);
    }
    ss << ";";
    return ss.str();
}

std::optional<std::pair<dex::u1 *, size_t>>
transformClass(const char *name, size_t classDataLen, const unsigned char *classData,
               dex::Writer::Allocator *allocator) {
    ALOGI("Trying to instrument class: %s", name);
    dex::Reader reader(classData, classDataLen);

    // Find the actual class amongst all the classData.
    dex::u4 index = reader.FindClassIndex(classNameToDescriptor(name).c_str());
    assert(index != dex::kNoIndex);
    reader.CreateClassIr(index);
    std::shared_ptr<ir::DexFile> ir = reader.GetIr();

    {
        Transformer transformer(ir);
        if (!transformer.transform()) {
            return std::nullopt;
        }
    }

    size_t new_size;
    dex::Writer writer(ir);
    dex::u1 *newClassData = writer.CreateImage(allocator, &new_size);
    return std::make_pair(newClassData, new_size);
}

void transformHook(jvmtiEnv *jvmtiEnv, JNIEnv *env,
                   jclass classBeingRedefined, jobject loader, const char *name,
                   jobject protectionDomain, jint classDataLen,
                   const unsigned char *classData, jint *newClassDataLen,
                   unsigned char **newClassData) {
    //ALOGI("transformHook(%s, loader=%px)", name, loader);

    // Don't instrument the instrumentation class
    if (strcmp(name, "com/ammaraskar/coverageagent/Instrumentation") == 0) {
        return;
    }

    //ALOGI("Transform hook called with name: %s", name);
    // Only instrument my own classes.
    if (strncmp("org/gts3/", name, 9) != 0) {
        return;
    }

    JvmtiAllocator allocator(jvmtiEnv);
    auto new_class = transformClass(name, classDataLen, classData, &allocator);

    if (new_class) {
        *newClassData = new_class->first;
        *newClassDataLen = new_class->second;
    }
}


void nativeFunctionHook();


class NativeHook {
public:
    // Constructor
    NativeHook(void *originalFunction, std::string functionName, char *methodSignature)
            : originalFunction(originalFunction), functionName(functionName),
              methodSignature(methodSignature) {
        createTrampoline();
    }

    void instrumentation() {
        ALOGI("Instrumentation called for %s", functionName.c_str());
        // TODO: Log full name (package + class + method + signature)
    }

    // Contains the original function, the function name, the method signature, and the trampoline
    void *originalFunction;
    std::string functionName;
    char *methodSignature;
    void *trampoline;

private:
    void createTrampoline() {
        // Allocate rwx page
        // TODO: Reuse pages
        void *trampoline = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (trampoline == MAP_FAILED) {
            ALOGE("Failed to allocate trampoline");
            exit(1);
        }

        // Write the trampoline code
        unsigned char *trampoline_code = reinterpret_cast<unsigned char *>(trampoline);

        /*
         * In the assembly, we want to:
         * 1. Save the argument registers
         * 2. Call the instrumentation
         * 3. Restore the argument registers
         * 4. Jump to the original function
         */

#ifdef __x86_64__
        // Push rax (Fix the stack alignment)
        trampoline_code[0] = 0x50;

        // Save the argument registers (push rdi, rsi, rdx, rcx, r8, r9)
        trampoline_code[1] = 0x57;
        trampoline_code[2] = 0x56;
        trampoline_code[3] = 0x52;
        trampoline_code[4] = 0x51;
        trampoline_code[5] = 0x41;
        trampoline_code[6] = 0x50;
        trampoline_code[7] = 0x41;
        trampoline_code[8] = 0x51;

        // Call the instrumentation
        // mov rax, <address>
        trampoline_code[9] = 0x48;
        trampoline_code[10] = 0xb8;
        *reinterpret_cast<void **>(&trampoline_code[11]) = reinterpret_cast<void *>(&nativeFunctionHook);
        // mov rbx, <this>
        trampoline_code[19] = 0x48;
        trampoline_code[20] = 0xbb;
        *reinterpret_cast<void **>(&trampoline_code[21]) = reinterpret_cast<void *>(this);
        // call rax
        trampoline_code[29] = 0xff;
        trampoline_code[30] = 0xd0;

        // Restore the argument registers (pop r9, r8, rcx, rdx, rsi, rdi)
        trampoline_code[31] = 0x41;
        trampoline_code[32] = 0x59;
        trampoline_code[33] = 0x41;
        trampoline_code[34] = 0x58;
        trampoline_code[35] = 0x59;
        trampoline_code[36] = 0x5a;
        trampoline_code[37] = 0x5e;
        trampoline_code[38] = 0x5f;

        // Add 8 to rsp (Fix the stack alignment)
        trampoline_code[39] = 0x48;
        trampoline_code[40] = 0x83;
        trampoline_code[41] = 0xc4;
        trampoline_code[42] = 0x08;

        // Jump to the original function
        // mov rax, <address>
        trampoline_code[43] = 0x48;
        trampoline_code[44] = 0xb8;
        *reinterpret_cast<void **>(&trampoline_code[45]) = originalFunction;
        // jmp rax
        trampoline_code[53] = 0xff;
        trampoline_code[54] = 0xe0;

#elif __aarch64__
        // TODO
#elif __i386__
        // TODO
#elif __arm__
        // TODO
#else
#error "Unsupported architecture"
#endif

        // Flush the instruction cache
        __builtin___clear_cache((char *)trampoline, (char *)trampoline + 4096);

        this->trampoline =  trampoline;
    }
};


void nativeFunctionHook() {
    NativeHook *nativeHook;
    // Retrieve the nativeHook
#ifdef __x86_64__
    __asm__ (
    "movq %%rbx, %0\n"
    : "=r" (nativeHook)
    );
#elif __aarch64__
    // TODO
#elif __i386__
    // TODO
#elif __arm__
    // TODO
#else
#error "Unsupported architecture"
#endif

    nativeHook->instrumentation();
}


void transformNativeHook(jvmtiEnv *jvmtiEnv, JNIEnv *env,
                         jthread thread, jmethodID method, void *address, void **new_address_ptr) {
    char *method_name;
    char *method_signature;
    jvmtiError error = jvmtiEnv->GetMethodName(method, &method_name, &method_signature, NULL);
    ALOGI("Hooking function: %s\n", method_name);

    if (strcmp(method_name, "toUpper") != 0)
        return;

    NativeHook *nativeHook = new NativeHook(address, method_name, method_signature);
    *new_address_ptr = reinterpret_cast<void *>(nativeHook->trampoline);
}

void addInstrumentationClassToClassPath(jvmtiEnv *jvmtiEnv, char* appDataDir) {
    auto dexFile = cmrc::dex_resources::get_filesystem().open("gen/Instrumentation.dex");

    // Put the dexFile on disk so we can add it to the classpath.
    std::string outputFileName = std::string(appDataDir) + "/code_cache/instrumentation.dex";
    ALOGI("Writing to file: %s", outputFileName.c_str());
    std::ofstream outputDexFile(outputFileName, std::ofstream::binary);
    std::copy(dexFile.begin(), dexFile.end(), std::ostreambuf_iterator<char>(outputDexFile));
    outputDexFile.close();
    ALOGI("File created. Adding to classpath");

    jvmtiEnv->AddToBootstrapClassLoaderSearch(outputFileName.c_str());
}

jvmtiEnv *CreateJvmtiEnv(JavaVM *vm) {
    jvmtiEnv *jvmti_env;
    jint result = vm->GetEnv((void **) &jvmti_env, JVMTI_VERSION_1_2);
    if (result != JNI_OK) {
        return nullptr;
    }

    return jvmti_env;
}

// Early attachment (e.g. 'java -agent[lib|path]:filename.so').
extern "C" JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *input,
                                                 void *reserved) {
    ALOGI("========== Agent_OnLoad Start =======");

    jvmtiEnv *env = CreateJvmtiEnv(vm);
    if (env == nullptr) {
        ALOGE("Unable to create jvmti env");
        return JNI_ERR;
    }

    jvmtiCapabilities caps = {0};
    caps.can_generate_native_method_bind_events = 1;
    caps.can_retransform_classes = 1;
    if (env->AddCapabilities(&caps) != JVMTI_ERROR_NONE) {
        ALOGE("Unable to add can_generate_native_method_bind_events capability");
        return JNI_ERR;
    }

    jvmtiEventCallbacks callbacks = {0};
    callbacks.ClassFileLoadHook = transformHook;
    callbacks.NativeMethodBind = transformNativeHook;
    if (env->SetEventCallbacks(&callbacks, sizeof(callbacks)) != JVMTI_ERROR_NONE) {
        ALOGE("Unable to set jvmti file load hook");
        return JNI_ERR;
    }
    if (env->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr) !=
        JVMTI_ERROR_NONE) {
        ALOGE("Unable to set event notification (JVMTI_EVENT_CLASS_FILE_LOAD_HOOK)");
        return JNI_ERR;
    }
    if (env->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, nullptr) !=
        JVMTI_ERROR_NONE) {
        ALOGE("Unable to set event notification (JVMTI_EVENT_VM_INIT)");
        return JNI_ERR;
    }
    if (env->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_NATIVE_METHOD_BIND, nullptr) !=
        JVMTI_ERROR_NONE) {
        ALOGE("Unable to set event notification (JVMTI_EVENT_NATIVE_METHOD_BIND)");
        return JNI_ERR;
    }

    // Add the instrumentation class to the classpath. The input passed in to startup_agents is the
    // app's data directory.
    addInstrumentationClassToClassPath(env, input);

    ALOGI("========== Agent_OnLoad End =======");

    return JNI_OK;
}

// Late attachment (e.g. 'am attach-agent').
JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char* options, void* reserved) {
    return Agent_OnLoad(vm, options, reserved);
}
