#include "BakeGlobalObject.h"
#include "BakeSourceProvider.h"
#include "JSNextTickQueue.h"
#include "JavaScriptCore/GlobalObjectMethodTable.h"
#include "JavaScriptCore/JSInternalPromise.h"
#include "headers-handwritten.h"
#include "JavaScriptCore/JSModuleLoader.h"
#include "JavaScriptCore/Completion.h"
#include "JavaScriptCore/JSSourceCode.h"

extern "C" BunString BakeProdResolve(JSC::JSGlobalObject*, BunString a, BunString b);

namespace Bake {
using namespace JSC;

JSC::JSInternalPromise*
bakeModuleLoaderImportModule(JSC::JSGlobalObject* global,
    JSC::JSModuleLoader* moduleLoader, JSC::JSString* moduleNameValue,
    JSC::JSValue parameters,
    const JSC::SourceOrigin& sourceOrigin)
{
    WTF::String keyString = moduleNameValue->getString(global);
    if (keyString.startsWith("bake:/"_s)) {
        auto& vm = JSC::getVM(global);
        return JSC::importModule(global, JSC::Identifier::fromString(vm, keyString),
            JSC::jsUndefined(), parameters, JSC::jsUndefined());
    }

    if (!sourceOrigin.isNull() && sourceOrigin.string().startsWith("bake:/"_s)) {
        auto& vm = JSC::getVM(global);
        auto scope = DECLARE_THROW_SCOPE(vm);

        WTF::String refererString = sourceOrigin.string();
        WTF::String keyString = moduleNameValue->getString(global);

        if (!keyString) {
            auto promise = JSC::JSInternalPromise::create(vm, global->internalPromiseStructure());
            promise->reject(global, JSC::createError(global, "import() requires a string"_s));
            return promise;
        }

        BunString result = BakeProdResolve(global, Bun::toString(refererString), Bun::toString(keyString));
        RETURN_IF_EXCEPTION(scope, nullptr);

        return JSC::importModule(global, JSC::Identifier::fromString(vm, result.toWTFString()),
            JSC::jsUndefined(), parameters, JSC::jsUndefined());
    }

    // TODO: make static cast instead of jscast
    // Use Zig::GlobalObject's function
    return jsCast<Zig::GlobalObject*>(global)->moduleLoaderImportModule(global, moduleLoader, moduleNameValue, parameters, sourceOrigin);
}

JSC::Identifier bakeModuleLoaderResolve(JSC::JSGlobalObject* jsGlobal,
    JSC::JSModuleLoader* loader, JSC::JSValue key,
    JSC::JSValue referrer, JSC::JSValue origin)
{
    Bake::GlobalObject* global = jsCast<Bake::GlobalObject*>(jsGlobal);
    auto& vm = JSC::getVM(global);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (auto string = jsDynamicCast<JSC::JSString*>(referrer)) {
        WTF::String refererString = string->getString(global);

        WTF::String keyString = key.toWTFString(global);
        RETURN_IF_EXCEPTION(scope, vm.propertyNames->emptyIdentifier);

        if (refererString.startsWith("bake:/"_s) || (refererString == "."_s && keyString.startsWith("bake:/"_s))) {
            BunString result = BakeProdResolve(global, Bun::toString(referrer.getString(global)), Bun::toString(keyString));
            RETURN_IF_EXCEPTION(scope, vm.propertyNames->emptyIdentifier);

            return JSC::Identifier::fromString(vm, result.toWTFString(BunString::ZeroCopy));
        }
    }

    if (auto string = jsDynamicCast<JSC::JSString*>(key)) {
        auto keyView = string->getString(global);
        RETURN_IF_EXCEPTION(scope, vm.propertyNames->emptyIdentifier);

        if (keyView.startsWith("bake:/"_s)) {
            BunString result = BakeProdResolve(global, Bun::toString("bake:/"_s), Bun::toString(keyView.substringSharingImpl("bake:"_s.length())));
            RETURN_IF_EXCEPTION(scope, vm.propertyNames->emptyIdentifier);

            return JSC::Identifier::fromString(vm, result.transferToWTFString());
        }
    }

    // Use Zig::GlobalObject's function
    return Zig::GlobalObject::moduleLoaderResolve(jsGlobal, loader, key, referrer, origin);
}

static JSC::JSInternalPromise* rejectedInternalPromise(JSC::JSGlobalObject* globalObject, JSC::JSValue value)
{
    auto& vm = JSC::getVM(globalObject);
    JSC::JSInternalPromise* promise = JSC::JSInternalPromise::create(vm, globalObject->internalPromiseStructure());
    promise->internalField(JSC::JSPromise::Field::ReactionsOrResult).set(vm, promise, value);
    promise->internalField(JSC::JSPromise::Field::Flags).set(vm, promise, JSC::jsNumber(promise->internalField(JSC::JSPromise::Field::Flags).get().asUInt32AsAnyInt() | JSC::JSPromise::isFirstResolvingFunctionCalledFlag | static_cast<unsigned>(JSC::JSPromise::Status::Rejected)));
    return promise;
}

static JSC::JSInternalPromise* resolvedInternalPromise(JSC::JSGlobalObject* globalObject, JSC::JSValue value)
{
    auto& vm = JSC::getVM(globalObject);
    JSC::JSInternalPromise* promise = JSC::JSInternalPromise::create(vm, globalObject->internalPromiseStructure());
    promise->internalField(JSC::JSPromise::Field::ReactionsOrResult).set(vm, promise, value);
    promise->internalField(JSC::JSPromise::Field::Flags).set(vm, promise, JSC::jsNumber(promise->internalField(JSC::JSPromise::Field::Flags).get().asUInt32AsAnyInt() | JSC::JSPromise::isFirstResolvingFunctionCalledFlag | static_cast<unsigned>(JSC::JSPromise::Status::Fulfilled)));
    return promise;
}

extern "C" BunString BakeProdLoad(ProductionPerThread* perThreadData, BunString a);

JSC::JSInternalPromise* bakeModuleLoaderFetch(JSC::JSGlobalObject* globalObject,
    JSC::JSModuleLoader* loader, JSC::JSValue key,
    JSC::JSValue parameters, JSC::JSValue script)
{
    Bake::GlobalObject* global = jsCast<Bake::GlobalObject*>(globalObject);
    auto& vm = JSC::getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto moduleKey = key.toWTFString(globalObject);
    if (scope.exception()) [[unlikely]]
        return rejectedInternalPromise(globalObject, scope.exception()->value());

    if (moduleKey.startsWith("bake:/"_s)) {
        if (global->m_perThreadData) [[likely]] {
            BunString source = BakeProdLoad(global->m_perThreadData, Bun::toString(moduleKey));
            if (source.tag != BunStringTag::Dead) {
                JSC::SourceOrigin origin = JSC::SourceOrigin(WTF::URL(moduleKey));
                JSC::SourceCode sourceCode = JSC::SourceCode(Bake::SourceProvider::create(
                    globalObject,
                    source.toWTFString(),
                    origin,
                    WTFMove(moduleKey),
                    WTF::TextPosition(),
                    JSC::SourceProviderSourceType::Module));
                return resolvedInternalPromise(globalObject, JSC::JSSourceCode::create(vm, WTFMove(sourceCode)));
            }
            return rejectedInternalPromise(globalObject, createTypeError(globalObject, makeString("Bundle does not have \""_s, moduleKey, "\". This is a bug in Bun's bundler."_s)));
        }
        return rejectedInternalPromise(globalObject, createTypeError(globalObject, "BakeGlobalObject does not have per-thread data configured"_s));
    }

    return Zig::GlobalObject::moduleLoaderFetch(globalObject, loader, key, parameters, script);
}

GlobalObject* GlobalObject::create(JSC::VM& vm, JSC::Structure* structure,
    const JSC::GlobalObjectMethodTable* methodTable)
{
    Bake::GlobalObject* ptr = new (NotNull, JSC::allocateCell<Bake::GlobalObject>(vm))
        Bake::GlobalObject(vm, structure, methodTable);
    ptr->finishCreation(vm);
    return ptr;
}

void GlobalObject::finishCreation(JSC::VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
}

JSC::Structure* GlobalObject::createStructure(JSC::VM& vm)
{
    auto* structure = JSC::Structure::create(vm, nullptr, jsNull(), JSC::TypeInfo(JSC::GlobalObjectType, StructureFlags & ~IsImmutablePrototypeExoticObject), info());
    structure->setTransitionWatchpointIsLikelyToBeFired(true);
    return structure;
}

struct BunVirtualMachine;
extern "C" BunVirtualMachine* Bun__getVM();

const JSC::GlobalObjectMethodTable& GlobalObject::globalObjectMethodTable()
{
    const auto& parent = Zig::GlobalObject::globalObjectMethodTable();
#define INHERIT_HOOK_METHOD(name) \
    parent.name

    static const JSC::GlobalObjectMethodTable table = {
        INHERIT_HOOK_METHOD(supportsRichSourceInfo),
        INHERIT_HOOK_METHOD(shouldInterruptScript),
        INHERIT_HOOK_METHOD(javaScriptRuntimeFlags),
        INHERIT_HOOK_METHOD(queueMicrotaskToEventLoop),
        INHERIT_HOOK_METHOD(shouldInterruptScriptBeforeTimeout),
        bakeModuleLoaderImportModule,
        bakeModuleLoaderResolve,
        bakeModuleLoaderFetch,
        INHERIT_HOOK_METHOD(moduleLoaderCreateImportMetaProperties),
        INHERIT_HOOK_METHOD(moduleLoaderEvaluate),
        INHERIT_HOOK_METHOD(promiseRejectionTracker),
        INHERIT_HOOK_METHOD(reportUncaughtExceptionAtEventLoop),
        INHERIT_HOOK_METHOD(currentScriptExecutionOwner),
        INHERIT_HOOK_METHOD(scriptExecutionStatus),
        INHERIT_HOOK_METHOD(reportViolationForUnsafeEval),
        INHERIT_HOOK_METHOD(defaultLanguage),
        INHERIT_HOOK_METHOD(compileStreaming),
        INHERIT_HOOK_METHOD(instantiateStreaming),
        INHERIT_HOOK_METHOD(deriveShadowRealmGlobalObject),
        INHERIT_HOOK_METHOD(codeForEval),
        INHERIT_HOOK_METHOD(canCompileStrings),
        INHERIT_HOOK_METHOD(trustedScriptStructure),
    };
#undef INHERIT_HOOK_METHOD
    return table;
}

// A lot of this function is taken from 'Zig__GlobalObject__create'
// TODO: remove this entire method
extern "C" GlobalObject* BakeCreateProdGlobal(void* console)
{
    JSC::VM& vm = JSC::VM::create(JSC::HeapType::Large).leakRef();
    vm.heap.acquireAccess();
    JSC::JSLockHolder locker(vm);
    BunVirtualMachine* bunVM = Bun__getVM();
    WebCore::JSVMClientData::create(&vm, bunVM);

    JSC::Structure* structure = Bake::GlobalObject::createStructure(vm);
    Bake::GlobalObject* global = Bake::GlobalObject::create(
        vm, structure, &Bake::GlobalObject::globalObjectMethodTable());
    if (!global)
        BUN_PANIC("Failed to create BakeGlobalObject");

    global->m_bunVM = bunVM;

    JSC::gcProtect(global);

    global->setConsole(console);
    global->setStackTraceLimit(10); // Node.js defaults to 10
    global->isThreadLocalDefaultGlobalObject = true;

    // TODO: it segfaults! process.nextTick is scoped out for now i guess!
    // vm.setOnComputeErrorInfo(computeErrorInfoWrapper);
    // vm.setOnEachMicrotaskTick([global](JSC::VM &vm) -> void {
    //   if (auto nextTickQueue = global->m_nextTickQueue.get()) {
    //     global->resetOnEachMicrotaskTick();
    //     // Bun::JSNextTickQueue *queue =
    //     //     jsCast<Bun::JSNextTickQueue *>(nextTickQueue);
    //     // queue->drain(vm, global);
    //     return;
    //   }
    // });

    return global;
}

extern "C" void BakeGlobalObject__attachPerThreadData(GlobalObject* global, ProductionPerThread* perThreadData)
{
    global->m_perThreadData = perThreadData;
}

const JSC::ClassInfo Bake::GlobalObject::s_info = { "GlobalObject"_s, &Base::s_info, nullptr, nullptr,
    CREATE_METHOD_TABLE(Bake::GlobalObject) };

}; // namespace Bake
