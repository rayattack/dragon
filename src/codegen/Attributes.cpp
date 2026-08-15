/// Dragon CodeGen - Attribute Access, Subscript, Slice
#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(AttributeExpr& node) {
    // `expr.__doc__`: each branch returns `i8*` (Optional[str] niche-ptr, D030/D031)
    // - `null` = None, non-null a `.rodata` C string flowing through the normal str machinery.
    if (node.attribute == "__doc__") {
        // 1. Module: `mod.__doc__` - emit (and cache) a `.rodata` constant
        //  for the module's lifted docstring; null when absent.
        if (node.object->type && node.object->type->kind() == Type::Kind::Module) {
            const std::string& modName =
                static_cast<ModuleType&>(*node.object->type).name;
            auto cIt = impl_->moduleDocConstants.find(modName);
            if (cIt != impl_->moduleDocConstants.end()) {
                impl_->lastValue = cIt->second;
                return;
            }
            auto dIt = impl_->moduleDocstrings.find(modName);
            if (dIt == impl_->moduleDocstrings.end()) {
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(impl_->i8PtrType));
                return;
            }
            auto* docStr = impl_->builder->CreateGlobalString(
                dIt->second, "module_doc_" + (modName.empty() ? "main" : modName));
            auto* casted = llvm::ConstantExpr::getBitCast(
                llvm::cast<llvm::Constant>(docStr), impl_->i8PtrType);
            impl_->moduleDocConstants[modName] = casted;
            impl_->lastValue = casted;
            return;
        }

        if (auto* objName = dynamic_cast<NameExpr*>(node.object.get())) {
            // 2. Top-level function `f.__doc__`: resolve via the same alias map
            // CallExpr/Assign use (`importedFuncAliasesByModule`), then current-module mangling.
            std::string mangled;
            std::string aliased = impl_->lookupImportedAlias(objName->name);
            if (!aliased.empty()) {
                mangled = aliased;
            } else {
                mangled = Impl::mangleFunc(impl_->currentModuleName, objName->name);
            }
            auto fcIt = impl_->functionDocConstants.find(mangled);
            if (fcIt != impl_->functionDocConstants.end()) {
                impl_->lastValue = fcIt->second;
                return;
            }
            auto fdIt = impl_->functionDocstrings.find(mangled);
            if (fdIt != impl_->functionDocstrings.end()) {
                auto* docStr = impl_->builder->CreateGlobalString(
                    fdIt->second, "func_doc_" + objName->name);
                auto* casted = llvm::ConstantExpr::getBitCast(
                    llvm::cast<llvm::Constant>(docStr), impl_->i8PtrType);
                impl_->functionDocConstants[mangled] = casted;
                impl_->lastValue = casted;
                return;
            }
            // Same-module lookup miss: if the bare name is a top-level fn in some
            // OTHER compiled module, that's the no-docstring case - return None per the niche-ptr ABI.
            if (impl_->module->getFunction(Impl::userFuncName(objName->name)) ||
                impl_->module->getFunction(mangled)) {
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(impl_->i8PtrType));
                return;
            }

            // 3. Class object: `Cls.__doc__` - load descriptor, call
            //  runtime accessor (returns const char* / null).
            if (impl_->classNames.count(objName->name)) {
                auto descIt = impl_->classDescriptorGlobalsBySym.find(impl_->classSym(objName->name));
                if (descIt != impl_->classDescriptorGlobalsBySym.end()) {
                    auto* descVal = impl_->builder->CreateLoad(
                        impl_->i64Type, descIt->second, objName->name + "_desc");
                    impl_->lastValue = impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_class_descriptor_get_doc"],
                        {descVal}, "class_doc");
                    return;
                }
            }
        }

        // 4. Instance `obj.__doc__`: dispatch to the runtime via
        // header.class_id -> descriptor -> doc.
        std::string instCls = impl_->resolveExprClassName(node.object.get());
        if (instCls.empty() && node.object->type &&
            node.object->type->kind() == Type::Kind::Class) {
            // Defensive: TypeChecker resolved a class type but resolveExprClassName
            // couldn't recover the name - fall through to evaluating the object below.
        }
        // Class/method __doc__ through a chained AttributeExpr (`mod.Cls.__doc__`,
        // `MyClass.method.__doc__`): methods aren't first-class, so pattern-match the chain directly and emit the cached `.rodata` constant.
        if (auto* innerAttr = dynamic_cast<AttributeExpr*>(node.object.get())) {
            // Method docstring: inner is `Owner.method`, Owner either a class name
            // (static-style) or an instance (resolves via type or varClassNames).
            std::string ownerCls;
            if (auto* innerName = dynamic_cast<NameExpr*>(innerAttr->object.get())) {
                if (impl_->classNames.count(innerName->name)) {
                    ownerCls = innerName->name;
                } else {
                    auto vit = impl_->varClassNames.find(innerName->name);
                    if (vit != impl_->varClassNames.end()) ownerCls = vit->second;
                }
            }
            if (ownerCls.empty()) {
                // TypeChecker may have already labelled the inner object.
                ownerCls = impl_->resolveExprClassName(innerAttr->object.get());
            }
            if (!ownerCls.empty()) {
                auto cIt = impl_->methodDocConstantsBySym.find(impl_->classSym(ownerCls));
                if (cIt != impl_->methodDocConstantsBySym.end()) {
                    auto mIt = cIt->second.find(innerAttr->attribute);
                    if (mIt != cIt->second.end()) {
                        impl_->lastValue = mIt->second;
                        return;
                    }
                }
                auto dIt = impl_->methodDocstringsBySym.find(impl_->classSym(ownerCls));
                if (dIt != impl_->methodDocstringsBySym.end()) {
                    auto mIt = dIt->second.find(innerAttr->attribute);
                    if (mIt != dIt->second.end()) {
                        auto* docStr = impl_->builder->CreateGlobalString(
                            mIt->second, ownerCls + "_" + innerAttr->attribute + "__doc");
                        auto* casted = llvm::ConstantExpr::getBitCast(
                            llvm::cast<llvm::Constant>(docStr), impl_->i8PtrType);
                        impl_->methodDocConstantsBySym[impl_->classSym(ownerCls)][innerAttr->attribute] = casted;
                        impl_->lastValue = casted;
                        return;
                    }
                    // Class known, method has no stashed docstring: return None
                    // ("method exists" is implicit - a missing method errors elsewhere).
                }
                // Class known, no method docstring stashed: return None per
                // the Optional[str] niche-ptr ABI.
                impl_->lastValue = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(impl_->i8PtrType));
                return;
            }
            if (innerAttr->type && innerAttr->type->kind() == Type::Kind::Class) {
                node.object->accept(*this);
                llvm::Value* descVal = impl_->lastValue;
                if (descVal->getType()->isPointerTy())
                    descVal = impl_->builder->CreatePtrToInt(descVal, impl_->i64Type);
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_class_descriptor_get_doc"],
                    {descVal}, "class_doc");
                return;
            }
        }
        if (!instCls.empty()) {
            node.object->accept(*this);
            llvm::Value* obj = impl_->lastValue;
            if (!obj->getType()->isPointerTy())
                obj = impl_->builder->CreateIntToPtr(obj, impl_->i8PtrType);
            else if (obj->getType() != impl_->i8PtrType)
                obj = impl_->builder->CreateBitCast(obj, impl_->i8PtrType);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_instance_get_doc"],
                {obj}, "inst_doc");
            return;
        }
        // No matching case: fall through to the normal "attribute not found"
        // diagnostic. The four branches above cover every documented __doc__ target.
    }

    // ModuleType base = a compile-time cross-module symbol ref; do NOT visit node.object
    // (modules have no runtime repr) - all modules share one flat symbol namespace, so the attribute name alone resolves it.
    if (node.object->type && node.object->type->kind() == Type::Kind::Module) {
        if (node.type && node.type->kind() == Type::Kind::Function) {
            // Resolve via the source module's mangled symbol; fall back to the bare
            // name for paths that still emit unmangled (extern-C, entry-module fn-as-value).
            const std::string& srcMod =
                static_cast<ModuleType&>(*node.object->type).name;
            llvm::Function* func = impl_->module->getFunction(
                Impl::mangleFunc(srcMod, node.attribute));
            if (!func) {
                func = impl_->module->getFunction(Impl::userFuncName(node.attribute));
            }
            if (func) {
                impl_->lastValue = func;
                return;
            }
            impl_->addError("module function '" + node.attribute + "' not found in linked module",
                            node.location());
            impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
            return;
        }
        if (node.type && node.type->kind() == Type::Kind::Class) {
            auto descIt = impl_->classDescriptorGlobalsBySym.find(
                impl_->classSym(node.attribute));
            if (descIt != impl_->classDescriptorGlobalsBySym.end()) {
                impl_->lastValue = impl_->builder->CreateLoad(
                    impl_->i64Type, descIt->second, node.attribute + "_desc");
                return;
            }
            impl_->addError("module class '" + node.attribute + "' not found in linked module",
                            node.location());
            impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
            return;
        }
        // Submodule-as-value (`let m = controllers.health`): a module has no runtime
        // representation, so fail loudly here rather than emit a null pointer.
        if (node.type && node.type->kind() == Type::Kind::Module) {
            impl_->addError("module '" + static_cast<ModuleType&>(*node.type).name +
                            "' has no runtime value", node.location());
            impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
            return;
        }
        // Qualified module-global load (`mod.SIGINT`): resolve in the TARGET
        // module's namespace via the ModuleType, mirroring the from-import path
        {
            const std::string& srcMod =
                static_cast<ModuleType&>(*node.object->type).name;
            auto gvIt = impl_->moduleGlobals.find(
                Impl::mangleGlobal(srcMod, node.attribute));
            if (gvIt != impl_->moduleGlobals.end()) {
                impl_->lastValue = impl_->builder->CreateLoad(
                    gvIt->second->getValueType(), gvIt->second, node.attribute);
                return;
            }
        }
        // Const or other export kind - fall through to existing logic for
        // now. Will need expansion as more module export kinds gain values.
    }

    // Static field access: ClassName.field (where ClassName is a known class, not an instance)
    if (auto* objName = dynamic_cast<NameExpr*>(node.object.get())) {
        auto sfIt = impl_->staticFieldGlobalsBySym.find(impl_->classSym(objName->name));
        if (sfIt != impl_->staticFieldGlobalsBySym.end()) {
            auto gvIt = sfIt->second.find(node.attribute);
            if (gvIt != sfIt->second.end()) {
                llvm::GlobalVariable* gv = gvIt->second;
                impl_->lastValue = impl_->builder->CreateLoad(
                    gv->getValueType(), gv, node.attribute);
                return;
            }
        }
    }

    // Dict dot-access (`data.name` -> typed dict get): fires when `data` is a
    // Dict-kind NameExpr, or an AttributeExpr resolving to a Dict-kind class field - else it falls to the struct-GEP path and reads garbage.
    if (impl_->isDragonFile) {
        // D039 Phase 9b: chained dot-access through a box (`cfg.server.port` where
        // cfg is dict[str, Any]) - extract the inner box's payload as a dict ptr (after tag-check) and reuse the dict-attr logic below.
        bool innerYieldsBox = false;
        if (auto* innerAttr = dynamic_cast<AttributeExpr*>(node.object.get())) {
            if (auto* iName = dynamic_cast<NameExpr*>(innerAttr->object.get())) {
                auto vit = impl_->varDictValueKinds.find(iName->name);
                if (vit != impl_->varDictValueKinds.end() &&
                    vit->second == Type::Kind::Any)
                    innerYieldsBox = true;
            }
        } else if (auto* innerSub = dynamic_cast<SubscriptExpr*>(node.object.get())) {
            if (auto* iName = dynamic_cast<NameExpr*>(innerSub->object.get())) {
                auto vit = impl_->varDictValueKinds.find(iName->name);
                if (vit != impl_->varDictValueKinds.end() &&
                    vit->second == Type::Kind::Any)
                    innerYieldsBox = true;
            }
        }
        if (innerYieldsBox) {
            node.object->accept(*this);
            llvm::Value* innerBox = impl_->lastValue;
            if (innerBox->getType() == impl_->boxType) {
                // Extract payload as a dict pointer. Tag check + TypeError on
                // mismatch matches Phase 7a's unbox-on-assign pattern.
                auto* func = impl_->currentFunction;
                auto* tag = impl_->boxTag(innerBox, "dot.tag");
                auto* expected = llvm::ConstantInt::get(impl_->i64Type, TAG_DICT); // TAG_DICT
                auto* match = impl_->builder->CreateICmpEQ(tag, expected, "dot.match");
                auto* okBB = llvm::BasicBlock::Create(*impl_->context, "dot.ok", func);
                auto* failBB = llvm::BasicBlock::Create(*impl_->context, "dot.fail", func);
                impl_->builder->CreateCondBr(match, okBB, failBB);

                impl_->builder->SetInsertPoint(failBB);
                auto* msg = impl_->builder->CreateGlobalString(
                    "TypeError: attribute access on non-dict Any value");
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_raise_exc_cstr"],
                    {llvm::ConstantInt::get(impl_->i64Type, 80), msg});
                impl_->builder->CreateUnreachable();

                impl_->builder->SetInsertPoint(okBB);
                auto* payloadI64 = impl_->boxPayloadI64(innerBox, "dot.payload");
                auto* dictPtr = impl_->builder->CreateIntToPtr(payloadI64, impl_->i8PtrType, "dot.dict");
                auto* keyStr = impl_->builder->CreateGlobalString(node.attribute);
                // Value type is Any (since the source dict was dict[str, Any],
                // its nested dict values are also Any-valued by spec).
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_box"],
                    {dictPtr, keyStr}, "dot.box.next");
                impl_->pendingDictCheckTag = -1;
                return;
            }
        }

        bool isDictObj = false;
        std::string dictObjName;          // for varDictValueKinds / TypedDict lookup
        std::string dictFieldClass;       // for classFieldDictValueKinds lookup
        std::string dictFieldName;
        std::string staticTypedDictClass; // TypedDict class via node.object->type (no var name)
        bool haveStaticDictValue = false; // dict[str,V] receiver resolved via static type
        Type::Kind staticDictValueKind = Type::Kind::Any;
        if (auto* objName = dynamic_cast<NameExpr*>(node.object.get())) {
            if (impl_->lookupVarKind(objName->name) == Impl::VarKind::Dict) {
                isDictObj = true;
                dictObjName = objName->name;
            }
        } else if (auto* objAttr = dynamic_cast<AttributeExpr*>(node.object.get())) {
            // Resolve the inner object's class.
            std::string cls;
            if (auto* attrObjName = dynamic_cast<NameExpr*>(objAttr->object.get())) {
                if (attrObjName->name == "self" && !impl_->currentClassName.empty())
                    cls = impl_->currentClassName;
                else {
                    auto vit = impl_->varClassNames.find(attrObjName->name);
                    if (vit != impl_->varClassNames.end()) cls = vit->second;
                }
            }
            if (cls.empty())
                cls = impl_->resolveExprClassName(objAttr->object.get());
            if (!cls.empty()) {
                auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(cls));
                if (fkIt != impl_->classFieldKindsBySym.end()) {
                    auto fkIt2 = fkIt->second.find(objAttr->attribute);
                    if (fkIt2 != fkIt->second.end() &&
                        fkIt2->second == Impl::VarKind::Dict) {
                        isDictObj = true;
                        dictFieldClass = cls;
                        dictFieldName = objAttr->attribute;
                    }
                }
            }
        }
        if (!isDictObj && node.object->type) {
            // Static-type fallback (D030): a receiver with no tracked var name
            // (`lst[0].key`, `make().key`) - else dot-access fell through to dragon_str_index and mis-read the dict as a string.
            auto sk = node.object->type->kind();
            if (sk == Type::Kind::Dict) {
                isDictObj = true;
                if (auto* dt = dynamic_cast<DictType*>(node.object->type.get())) {
                    if (dt->valueType) {
                        haveStaticDictValue = true;
                        staticDictValueKind = dt->valueType->kind();
                    }
                }
            } else if (sk == Type::Kind::Instance) {
                if (auto* inst = dynamic_cast<InstanceType*>(node.object->type.get())) {
                    if (inst->classType && inst->classType->isTypedDict) {
                        isDictObj = true;
                        staticTypedDictClass = inst->classType->name;
                    }
                }
            }
        }
        if (isDictObj) {
            node.object->accept(*this);
            llvm::Value* dict = impl_->lastValue;
            auto* keyStr = impl_->builder->CreateGlobalString(node.attribute);

            // D039 Phase 2 (dot-access mirror): dict value type Any routes to
            // dragon_dict_get_box (isinstance/print/unbox parity with `cfg["k"]`); a concrete-typed LHS still uses dragon_dict_get_checked below.
            bool valueIsAny = false;
            if (!dictObjName.empty()) {
                auto vit = impl_->varDictValueKinds.find(dictObjName);
                if (vit != impl_->varDictValueKinds.end() &&
                    vit->second == Type::Kind::Any)
                    valueIsAny = true;
            }
            if (!valueIsAny && !dictFieldClass.empty()) {
                auto cit = impl_->classFieldDictValueKindsBySym.find(impl_->classSym(dictFieldClass));
                if (cit != impl_->classFieldDictValueKindsBySym.end()) {
                    auto fit = cit->second.find(dictFieldName);
                    if (fit != cit->second.end() &&
                        fit->second == Type::Kind::Any)
                        valueIsAny = true;
                }
            }
            if (!valueIsAny && haveStaticDictValue &&
                staticDictValueKind == Type::Kind::Any)
                valueIsAny = true;
            if (valueIsAny && impl_->pendingDictCheckTag < 0) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_box"],
                    {dict, keyStr}, "dictdot.box");
                impl_->pendingDictCheckTag = -1;
                return;
            }

            // Determine checked tag for the typed-dispatch path.
            int64_t checkTag = impl_->pendingDictCheckTag;
            // Capture + clear the list-representation companion (see
            // pendingListViewElemTag); consumed by the tag-5 ptr branch below.
            int64_t pendingListElem = impl_->pendingListViewElemTag;
            impl_->pendingListViewElemTag = Impl::kNoListElemCheck;
            if (checkTag < 0 && !dictObjName.empty()) {
                auto tdIt = impl_->varTypedDictClass.find(dictObjName);
                if (tdIt != impl_->varTypedDictClass.end()) {
                    auto schemaIt = impl_->typedDictFieldKindsBySym.find(impl_->classSym(tdIt->second));
                    if (schemaIt != impl_->typedDictFieldKindsBySym.end()) {
                        auto fIt = schemaIt->second.find(node.attribute);
                        if (fIt != schemaIt->second.end())
                            checkTag = Impl::typeKindToTag(fIt->second);
                    }
                }
            }
            // D030 Phase 3.F: checkTag from the dict's tracked V kind via typeKindToTag
            // (Instance -> 7, matching the store side; typeKindToElemTag's Instance -> 5 is the list domain and would mismatch).
            if (checkTag < 0 && !dictObjName.empty()) {
                auto vit = impl_->varDictValueKinds.find(dictObjName);
                if (vit != impl_->varDictValueKinds.end())
                    checkTag = Impl::typeKindToTag(vit->second);
            }
            // Class-field dict: classFieldDictValueKinds tracks the V type.
            if (checkTag < 0 && !dictFieldClass.empty()) {
                auto cit = impl_->classFieldDictValueKindsBySym.find(impl_->classSym(dictFieldClass));
                if (cit != impl_->classFieldDictValueKindsBySym.end()) {
                    auto fit = cit->second.find(dictFieldName);
                    if (fit != cit->second.end()) {
                        int64_t t = Impl::typeKindToTag(fit->second);
                        if (t >= 0) checkTag = t;
                    }
                }
            }
            // Static TypedDict receiver (e.g. lst[0].field): field kind -> tag.
            if (checkTag < 0 && !staticTypedDictClass.empty()) {
                auto schemaIt = impl_->typedDictFieldKindsBySym.find(impl_->classSym(staticTypedDictClass));
                if (schemaIt != impl_->typedDictFieldKindsBySym.end()) {
                    auto fIt = schemaIt->second.find(node.attribute);
                    if (fIt != schemaIt->second.end())
                        checkTag = Impl::typeKindToTag(fIt->second);
                }
            }
            // Static dict[str,V] receiver: derive tag from the value kind
            // (typeKindToTag - the dict-value tag domain, Instance -> 7).
            if (checkTag < 0 && haveStaticDictValue) {
                checkTag = Impl::typeKindToTag(staticDictValueKind);
            }

            // D030 Phase 3.F: dispatch by checkTag - see SubscriptExpr.
            if (checkTag == TAG_FLOAT) {  // TAG_FLOAT
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_str_f64"], {dict, keyStr}, "dictdot.f");
                impl_->pendingDictCheckTag = -1;
            } else if (checkTag == 1 || checkTag == 5 || checkTag == 6 || checkTag == 7) {
                auto* tagVal = llvm::ConstantInt::get(impl_->i64Type, checkTag);
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_str_ptr"], {dict, keyStr, tagVal}, "dictdot.p");
                impl_->pendingDictCheckTag = -1;
                if (checkTag == 5 && pendingListElem != Impl::kNoListElemCheck)
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_view_check"],
                        {impl_->lastValue,
                         llvm::ConstantInt::get(impl_->i64Type, pendingListElem)});
            } else if (checkTag >= 0) {
                auto* tagVal = llvm::ConstantInt::get(impl_->i64Type, checkTag);
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get_checked"], {dict, keyStr, tagVal}, "dictdot_chk");
                impl_->pendingDictCheckTag = -1;
            } else {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_get"], {dict, keyStr}, "dictdot");
            }
            return;
        }
    }

    // Class field access: self.x or instance.x
    if (auto* objName = dynamic_cast<NameExpr*>(node.object.get())) {
        std::string className;
        if (objName->name == "self" && !impl_->currentClassName.empty()) {
            className = impl_->currentClassName;
        } else {
            auto vit = impl_->varClassNames.find(objName->name);
            if (vit != impl_->varClassNames.end()) className = vit->second;
        }

        // 4.1 @property: bare attribute access invokes the getter method.
        // Walk the inheritance chain so subclasses inherit parent properties.
        if (!className.empty()) {
            std::string getterClass;
            for (std::string cur = className; !cur.empty(); ) {
                auto pit = impl_->classPropertiesBySym.find(impl_->classSym(cur));
                if (pit != impl_->classPropertiesBySym.end() && pit->second.count(node.attribute)) {
                    getterClass = cur;
                    break;
                }
                auto pp = impl_->classParentNamesBySym.find(impl_->classSym(cur));
                if (pp == impl_->classParentNamesBySym.end()) break;
                cur = pp->second;
            }
            if (!getterClass.empty()) {
                // Mangle with the property's OWNING module: a cross-module class's
                // getter is `<mod>__<Class>_<attr>`, not the entry-module-only bare form.
                std::string getterFuncName =
                    impl_->classSymPrefix(getterClass) + "_" + node.attribute;
                auto* getterFn = impl_->module->getFunction(getterFuncName);
                if (getterFn) {
                    node.object->accept(*this);
                    llvm::Value* obj = impl_->lastValue;
                    if (!obj->getType()->isPointerTy())
                        obj = impl_->builder->CreateIntToPtr(obj, impl_->i8PtrType);
                    impl_->lastValue = impl_->normalizeIntC(
                        impl_->builder->CreateCall(getterFn, {obj}, "propget"));
                    return;
                }
            }
        }

        if (!className.empty()) {
            auto structIt = impl_->classStructTypesBySym.find(impl_->classSym(className));
            auto fieldIt = impl_->classFieldIndicesBySym.find(impl_->classSym(className));
            if (structIt != impl_->classStructTypesBySym.end() && fieldIt != impl_->classFieldIndicesBySym.end()) {
                auto idxIt = fieldIt->second.find(node.attribute);
                if (idxIt != fieldIt->second.end()) {
                    // Load object pointer
                    node.object->accept(*this);
                    llvm::Value* objPtr = impl_->lastValue;
                    // GEP to field + load
                    auto* gep = impl_->builder->CreateStructGEP(
                        structIt->second, objPtr, idxIt->second, node.attribute + "_ptr");
                    llvm::Type* fieldType = nullptr;
                    auto typesIt = impl_->classFieldTypesBySym.find(impl_->classSym(className));
                    if (typesIt != impl_->classFieldTypesBySym.end()) {
                        auto ftIt = typesIt->second.find(node.attribute);
                        if (ftIt != typesIt->second.end()) fieldType = ftIt->second;
                    }
                    if (!fieldType) {
                        impl_->addError(
                            "internal error: field type for '" + className + "." +
                            node.attribute + "' is missing from the class layout",
                            node.location());
                        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
                        return;
                    }
                    impl_->lastValue = impl_->builder->CreateLoad(fieldType, gep, node.attribute);
                    return;
                }
            }
        }
    }

    // Non-NameExpr object (`pos_specs[j].name`): resolve the class via
    // resolveExprClassName and emit a real field GEP, else the access silently returns i64 0 and poisons downstream uses.
    {
        std::string className = impl_->resolveExprClassName(node.object.get());
        if (!className.empty()) {
            // @property on an expression result (`p.parent.name`, `f().prop`):
            // invoke its getter, mirroring the bare-NameExpr property path above.
            std::string getterClass;
            for (std::string cur = className; !cur.empty(); ) {
                auto pit = impl_->classPropertiesBySym.find(impl_->classSym(cur));
                if (pit != impl_->classPropertiesBySym.end() && pit->second.count(node.attribute)) {
                    getterClass = cur;
                    break;
                }
                auto pp = impl_->classParentNamesBySym.find(impl_->classSym(cur));
                if (pp == impl_->classParentNamesBySym.end()) break;
                cur = pp->second;
            }
            if (!getterClass.empty()) {
                std::string getterFuncName =
                    impl_->classSymPrefix(getterClass) + "_" + node.attribute;
                if (auto* getterFn = impl_->module->getFunction(getterFuncName)) {
                    node.object->accept(*this);
                    llvm::Value* obj = impl_->lastValue;
                    if (!obj->getType()->isPointerTy())
                        obj = impl_->builder->CreateIntToPtr(obj, impl_->i8PtrType);
                    impl_->lastValue = impl_->normalizeIntC(
                        impl_->builder->CreateCall(getterFn, {obj}, "propget"));
                    // `f().prop`: the getter's result is its own +1; the
                    // owned receiver temp is fully consumed - release it.
                    Impl::VarKind rd =
                        impl_->ownedTempDrainKind(node.object.get(), obj);
                    if (rd != Impl::VarKind::Other)
                        impl_->emitDecrefByKind(obj, rd);
                    return;
                }
            }
            auto structIt = impl_->classStructTypesBySym.find(impl_->classSym(className));
            auto fieldIt = impl_->classFieldIndicesBySym.find(impl_->classSym(className));
            if (structIt != impl_->classStructTypesBySym.end() && fieldIt != impl_->classFieldIndicesBySym.end()) {
                auto idxIt = fieldIt->second.find(node.attribute);
                if (idxIt != fieldIt->second.end()) {
                    node.object->accept(*this);
                    llvm::Value* objPtr = impl_->lastValue;
                    if (!objPtr->getType()->isPointerTy())
                        objPtr = impl_->builder->CreateIntToPtr(objPtr, impl_->i8PtrType);
                    auto* gep = impl_->builder->CreateStructGEP(
                        structIt->second, objPtr, idxIt->second, node.attribute + "_ptr");
                    llvm::Type* fieldType = nullptr;
                    auto typesIt = impl_->classFieldTypesBySym.find(impl_->classSym(className));
                    if (typesIt != impl_->classFieldTypesBySym.end()) {
                        auto ftIt = typesIt->second.find(node.attribute);
                        if (ftIt != typesIt->second.end()) fieldType = ftIt->second;
                    }
                    if (!fieldType) {
                        impl_->addError(
                            "internal error: field type for '" + className + "." +
                            node.attribute + "' is missing from the class layout",
                            node.location());
                        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
                        return;
                    }
                    impl_->lastValue = impl_->builder->CreateLoad(fieldType, gep, node.attribute);
                    // `f().attr` (audit 1.7): retain the field BY KIND first, then
                    // release the receiver (retain-before-release so the field can't dangle) - fixes the one-Cookie-per-call `jar.get_cookie(k).value` leak.
                    Impl::VarKind rd =
                        impl_->ownedTempDrainKind(node.object.get(), objPtr);
                    if (rd != Impl::VarKind::Other) {
                        Impl::VarKind fk = Impl::VarKind::Other;
                        auto cfk = impl_->classFieldKindsBySym.find(impl_->classSym(className));
                        if (cfk != impl_->classFieldKindsBySym.end()) {
                            auto ff = cfk->second.find(node.attribute);
                            if (ff != cfk->second.end()) fk = ff->second;
                        }
                        llvm::Value* v = impl_->lastValue;
                        if (fk == Impl::VarKind::Str && v->getType()->isPointerTy()) {
                            auto* retainFn = impl_->getOrDeclareRuntime(
                                "dragon_str_retain",
                                llvm::FunctionType::get(impl_->i8PtrType,
                                                        {impl_->i8PtrType}, false));
                            impl_->lastValue = impl_->builder->CreateCall(
                                retainFn, {impl_->toI8Ptr(v)}, "attr.retain");
                        } else if (fk == Impl::VarKind::Closure &&
                                   v->getType()->isPointerTy()) {
                            impl_->builder->CreateCall(
                                impl_->runtimeFuncs["dragon_incref_callable"],
                                {impl_->toI8Ptr(v)});
                        } else if (fk == Impl::VarKind::Union &&
                                   v->getType() == impl_->boxType) {
                            impl_->emitUnionIncref(
                                impl_->boxPayloadI64(v, "attr.p"),
                                impl_->boxTag(v, "attr.t"));
                        } else if (Impl::isHeapKind(fk) &&
                                   v->getType()->isPointerTy()) {
                            auto* retainFn = impl_->getOrDeclareRuntime(
                                "dragon_obj_retain",
                                llvm::FunctionType::get(impl_->i8PtrType,
                                                        {impl_->i8PtrType}, false));
                            impl_->lastValue = impl_->builder->CreateCall(
                                retainFn, {impl_->toI8Ptr(v)}, "attr.retain");
                        }
                        impl_->emitDecrefByKind(objPtr, rd);
                    }
                    return;
                }
            }
        }
    }

    std::string recv = "<expression>";
    if (auto* on = dynamic_cast<NameExpr*>(node.object.get()))
        recv = "'" + on->name + "'";
    impl_->addError(
        "cannot resolve attribute '" + node.attribute + "' on receiver " + recv +
        ": no codegen dispatch path matched, so the access would have "
        "silently produced 0",
        node.location());
    impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
}
void CodeGen::visit(SubscriptExpr& node) {
    // __getitem__ dunder dispatch for class instances
    std::string subClassName = impl_->resolveExprClassName(node.object.get());
    if (!subClassName.empty() && impl_->hasDunder(subClassName, "__getitem__")) {
        node.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        std::vector<llvm::Value*> dunderBases;
        impl_->pushTempCleanupByKind(
            obj, impl_->ownedTempDrainKind(node.object.get(), obj), dunderBases);
        node.index->accept(*this);
        llvm::Value* idx = impl_->lastValue;
        impl_->lastValue = impl_->callDunder(subClassName, "__getitem__", obj, {idx});
        impl_->popArgTempCleanups(dunderBases);
        return;
    }

    // Check if index is a SliceExpr
    if (auto* slice = dynamic_cast<SliceExpr*>(node.index.get())) {
        node.object->accept(*this);
        llvm::Value* obj = impl_->lastValue;
        Impl::VarKind sliceRecvDrain =
            impl_->ownedTempDrainKind(node.object.get(), obj);
        std::vector<llvm::Value*> sliceBases;
        impl_->pushTempCleanupByKind(obj, sliceRecvDrain, sliceBases);

        // INT64_MIN sentinel for omitted bounds
        llvm::Value* sentinel = llvm::ConstantInt::get(impl_->i64Type, INT64_MIN);
        llvm::Value* lower = sentinel;
        llvm::Value* upper = sentinel;
        llvm::Value* step = llvm::ConstantInt::get(impl_->i64Type, 1);

        if (slice->lower) { slice->lower->accept(*this); lower = impl_->lastValue; }
        if (slice->upper) { slice->upper->accept(*this); upper = impl_->lastValue; }
        if (slice->step)  { slice->step->accept(*this);  step = impl_->lastValue; }

        bool isList = dynamic_cast<ListExpr*>(node.object.get()) != nullptr;
        if (!isList) {
            if (auto* nameExpr = dynamic_cast<NameExpr*>(node.object.get())) {
                isList = impl_->lookupVarKind(nameExpr->name) == Impl::VarKind::List;
            }
        }
        // D030 §5: bytes-detection has to happen first, AND has to override
        // list-detection - bytes slots collapse onto VarKind::List.
        bool isBytes = impl_->exprIsBytes(node.object.get());
        if (isBytes) isList = false;
        // Fallback: typechecker-propagated type, for chained subscripts (a[i][j],
        // d["k"][i]) and any other expression statically a list/dict/tuple.
        if (!isList && !isBytes && node.object->type &&
            node.object->type->kind() == Type::Kind::List) {
            isList = true;
        }

        if (isList) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_slice"], {obj, lower, upper, step}, "listslice");
        } else if (isBytes) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_bytes_slice"], {obj, lower, upper, step}, "bytesslice");
        } else {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_str_slice"], {obj, lower, upper, step}, "strslice");
        }
        // A slice COPIES (fresh +1, independent of the receiver): an OWNED receiver
        // temp (`("Z"+p)[1:3]`) is fully consumed here and must be released or it leaks once per evaluation (audit 1.7).
        impl_->popArgTempCleanups(sliceBases);
        if (sliceRecvDrain != Impl::VarKind::Other)
            impl_->emitDecrefByKind(obj, sliceRecvDrain);
        return;
    }

    // Check for dict subscript: d["key"] or self.field["key"]
    bool isDict = dynamic_cast<DictExpr*>(node.object.get()) != nullptr;
    if (!isDict) {
        if (auto* nameExpr = dynamic_cast<NameExpr*>(node.object.get())) {
            isDict = impl_->lookupVarKind(nameExpr->name) == Impl::VarKind::Dict;
        }
    }
    if (!isDict) {
        if (auto* attrExpr = dynamic_cast<AttributeExpr*>(node.object.get())) {
            std::string className;
            if (auto* objName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                if (objName->name == "self" && !impl_->currentClassName.empty()) {
                    className = impl_->currentClassName;
                } else {
                    auto vit = impl_->varClassNames.find(objName->name);
                    if (vit != impl_->varClassNames.end()) className = vit->second;
                }
            }
            if (!className.empty()) {
                auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(className));
                if (fkIt != impl_->classFieldKindsBySym.end()) {
                    auto fkIt2 = fkIt->second.find(attrExpr->attribute);
                    if (fkIt2 != fkIt->second.end() && fkIt2->second == Impl::VarKind::Dict)
                        isDict = true;
                }
            }
        }
    }
    // Fallback: typechecker-propagated type. Handles chained subscripts.
    if (!isDict && node.object->type &&
        node.object->type->kind() == Type::Kind::Dict) {
        isDict = true;
    }

    if (isDict) {
        // D030 Phase 3.G: route int-keyed dict reads through dragon_dict_int_*.
        // The key kind is resolved from the dict's tracked annotation before evaluating the index, picking the entry point in one pass.
        Type::Kind dictKk = impl_->resolveDictKeyKind(node.object.get());
        bool intKeyed = dictKk == Type::Kind::Int || dictKk == Type::Kind::Float;

        node.object->accept(*this);
        llvm::Value* dict = impl_->lastValue;
        Impl::VarKind recvDrain =
            impl_->ownedTempDrainKind(node.object.get(), dict);
        std::vector<llvm::Value*> subBases;
        impl_->pushTempCleanupByKind(dict, recvDrain, subBases);
        node.index->accept(*this);
        llvm::Value* key = impl_->lastValue;

        // An OWNED str KEY temp (`d[p + "x"]`) is fully consumed by the lookup (a
        // read never retains the key), so release it after or it leaks once per lookup. Captured before the int-key coercions overwrite `key`.
        llvm::Value* keyOrig = key;
        Impl::VarKind keyDrain =
            impl_->ownedTempDrainKind(node.index.get(), key);
        impl_->pushTempCleanupByKind(keyOrig, keyDrain, subBases);
        auto releaseOwnedKeyTemp = [&]() {
            if (keyDrain != Impl::VarKind::Other)
                impl_->emitDecrefByKind(keyOrig, keyDrain);
        };
        // An OWNED dict RECEIVER temp (`loads_ints()["k"]`) is fully consumed by a
        // PROVABLY-SCALAR value read, so release the whole temp dict or it leaks once per call (audit 1.7).
        auto releaseOwnedRecvTempScalar = [&](int64_t tag) {
            // tag 0/2/3 = int/float/bool: value copied, receiver done.
            if ((tag == 0 || tag == 2 || tag == 3) &&
                recvDrain != Impl::VarKind::Other)
                impl_->emitDecrefByKind(dict, recvDrain);
        };
        // A PTR value from an owned receiver temp (`r.info()["k"]`) borrows the
        // element FROM the receiver (audit 1.7's bounded leak): retain the element BY KIND first, THEN release the receiver, so it can never dangle.
        auto retainElemThenReleaseRecv = [&](int64_t tag) {
            if (recvDrain == Impl::VarKind::Other) return;
            llvm::Value* v = impl_->lastValue;
            if (v && v->getType()->isPointerTy()) {
                if (tag == 10) {
                    // Closure: incref-in-place (handles bare fn pointers);
                    // no returning retain exists for callables.
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_incref_callable"],
                        {impl_->toI8Ptr(v)});
                } else {
                    auto* retainFn = impl_->getOrDeclareRuntime(
                        tag == 1 ? "dragon_str_retain" : "dragon_obj_retain",
                        llvm::FunctionType::get(impl_->i8PtrType,
                                                {impl_->i8PtrType}, false));
                    impl_->lastValue = impl_->builder->CreateCall(
                        retainFn, {impl_->toI8Ptr(v)}, "dictget.retain");
                }
            }
            impl_->emitDecrefByKind(dict, recvDrain);
        };

        // Determine checked tag: pendingDictCheckTag (from AnnAssignStmt) or TypedDict schema
        int64_t checkTag = impl_->pendingDictCheckTag;
        // Capture + clear the list-representation companion (see
        // pendingListViewElemTag): consumed below by the tag-5 ptr branches.
        int64_t pendingListElem = impl_->pendingListViewElemTag;
        impl_->pendingListViewElemTag = Impl::kNoListElemCheck;
        if (checkTag < 0) {
            // Check if dict variable is a TypedDict with known schema
            if (auto* objName = dynamic_cast<NameExpr*>(node.object.get())) {
                auto tdIt = impl_->varTypedDictClass.find(objName->name);
                if (tdIt != impl_->varTypedDictClass.end()) {
                    // Try to get field name from string literal key
                    if (auto* strKey = dynamic_cast<StringLiteral*>(node.index.get())) {
                        std::string fieldName = strKey->value;
                        if (fieldName.size() >= 2 && (fieldName.front() == '"' || fieldName.front() == '\''))
                            fieldName = fieldName.substr(1, fieldName.size() - 2);
                        auto schemaIt = impl_->typedDictFieldKindsBySym.find(impl_->classSym(tdIt->second));
                        if (schemaIt != impl_->typedDictFieldKindsBySym.end()) {
                            auto fIt = schemaIt->second.find(fieldName);
                            if (fIt != schemaIt->second.end())
                                checkTag = Impl::typeKindToTag(fIt->second);
                        }
                    }
                }
            }
        }
        // D030 Phase 3.F: checkTag from the dict's tracked V kind (varDictValueKinds)
        // via typeKindToTag (Instance -> 7, matching the store side; typeKindToElemTag's Instance -> 5 is the list domain and would mismatch).
        if (checkTag < 0) {
            if (auto* objName = dynamic_cast<NameExpr*>(node.object.get())) {
                auto vit = impl_->varDictValueKinds.find(objName->name);
                if (vit != impl_->varDictValueKinds.end())
                    checkTag = Impl::typeKindToTag(vit->second);
            }
        }
        // Fallback: derive checkTag from the typechecker's DictType valueType, for
        // chained dict subscripts (d["k1"]["k2"]) that varDictValueKinds doesn't track.
        if (checkTag < 0 && node.object->type &&
            node.object->type->kind() == Type::Kind::Dict) {
            if (auto* dt = dynamic_cast<DictType*>(node.object->type.get())) {
                if (dt->valueType) {
                    int64_t t = Impl::typeKindToTag(dt->valueType->kind());
                    if (t >= 0) checkTag = t;
                }
            }
        }
        // Class-field dict subscript (`obj.field["k"]`): __init__'s `self.x: T = ...`
        // isn't typechecked onto the field, so node.object->type is Unknown - fall back to classFieldDictValueKinds (scanned from __init__ bodies, see Classes.cpp).
        if (checkTag < 0) {
            if (auto* attrExpr = dynamic_cast<AttributeExpr*>(node.object.get())) {
                std::string className;
                if (auto* objName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                    if (objName->name == "self" && !impl_->currentClassName.empty())
                        className = impl_->currentClassName;
                    else {
                        auto vit = impl_->varClassNames.find(objName->name);
                        if (vit != impl_->varClassNames.end()) className = vit->second;
                    }
                }
                if (className.empty())
                    className = impl_->resolveExprClassName(attrExpr->object.get());
                if (!className.empty()) {
                    auto cit = impl_->classFieldDictValueKindsBySym.find(impl_->classSym(className));
                    if (cit != impl_->classFieldDictValueKindsBySym.end()) {
                        auto fit = cit->second.find(attrExpr->attribute);
                        if (fit != cit->second.end()) {
                            int64_t t = Impl::typeKindToTag(fit->second);
                            if (t >= 0) checkTag = t;
                        }
                    }
                }
            }
        }

        // D039 Phase 2: a dict[K, Any] value type returns a {tag, payload} box, so
        // the receiver preserves tag info for isinstance narrowing/print/unbox-on-assign.
        bool valueIsAny = false;
        if (auto* objName = dynamic_cast<NameExpr*>(node.object.get())) {
            auto vit = impl_->varDictValueKinds.find(objName->name);
            if (vit != impl_->varDictValueKinds.end() &&
                vit->second == Type::Kind::Any)
                valueIsAny = true;
        }
        if (!valueIsAny && node.object->type &&
            node.object->type->kind() == Type::Kind::Dict) {
            if (auto* dt = dynamic_cast<DictType*>(node.object->type.get())) {
                if (dt->valueType && dt->valueType->kind() == Type::Kind::Any)
                    valueIsAny = true;
            }
        }
        if (!valueIsAny) {
            if (auto* attrExpr = dynamic_cast<AttributeExpr*>(node.object.get())) {
                std::string className;
                if (auto* objName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                    if (objName->name == "self" && !impl_->currentClassName.empty())
                        className = impl_->currentClassName;
                    else {
                        auto vit = impl_->varClassNames.find(objName->name);
                        if (vit != impl_->varClassNames.end()) className = vit->second;
                    }
                }
                if (className.empty())
                    className = impl_->resolveExprClassName(attrExpr->object.get());
                if (!className.empty()) {
                    auto cit = impl_->classFieldDictValueKindsBySym.find(impl_->classSym(className));
                    if (cit != impl_->classFieldDictValueKindsBySym.end()) {
                        auto fit = cit->second.find(attrExpr->attribute);
                        if (fit != cit->second.end() &&
                            fit->second == Type::Kind::Any)
                            valueIsAny = true;
                    }
                }
            }
        }

        // D039 Phase 2: str-keyed dict[str, Any] -> box-returning op, only when the
        // LHS expects a box (checkTag < 0); a concrete LHS (`x: int = d[k]`) falls through to the existing checked-get path instead.
        if (valueIsAny && !intKeyed && checkTag < 0) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_get_box"], {dict, key},
                "dictget.box");
            impl_->pendingDictCheckTag = -1;
            impl_->popArgTempCleanups(subBases);
            releaseOwnedKeyTemp();
            return;
        }

        // D030 Phase 3.G: int-keyed dispatch - same checkTag matrix but the
        // dragon_dict_int_* family. Key crosses at i64 (its native type).
        if (intKeyed) {
            if (dictKk == Type::Kind::Float)
                key = impl_->emitFloatDictKeyBits(key);
            if (key->getType() == impl_->i1Type)
                key = impl_->builder->CreateZExt(key, impl_->i64Type);
            else if (key->getType()->isPointerTy())
                key = impl_->builder->CreatePtrToInt(key, impl_->i64Type);
            else if (key->getType() != impl_->i64Type)
                key = impl_->builder->CreateZExtOrTrunc(key, impl_->i64Type);

            int64_t recvReleaseTag = -1;
            if (checkTag == 2) {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_int_get_f64"], {dict, key}, "dictget.if");
                impl_->pendingDictCheckTag = -1;
                recvReleaseTag = 2;
            } else if (checkTag == 1 || checkTag == 5 || checkTag == 6 || checkTag == 7 ||
                       checkTag == TAG_CALLABLE) {  // TAG_CLOSURE - return the closure as a ptr
                auto* tagVal = llvm::ConstantInt::get(impl_->i64Type, checkTag);
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_int_get_ptr"], {dict, key, tagVal}, "dictget.ip");
                impl_->pendingDictCheckTag = -1;
                if (checkTag == 5 && pendingListElem != Impl::kNoListElemCheck)
                    impl_->builder->CreateCall(
                        impl_->runtimeFuncs["dragon_list_view_check"],
                        {impl_->lastValue,
                         llvm::ConstantInt::get(impl_->i64Type, pendingListElem)});
                retainElemThenReleaseRecv(checkTag);
            } else if (checkTag >= 0) {
                auto* tagVal = llvm::ConstantInt::get(impl_->i64Type, checkTag);
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_int_get_checked"], {dict, key, tagVal}, "dictget.ichk");
                impl_->pendingDictCheckTag = -1;
                recvReleaseTag = checkTag;
            } else {
                impl_->lastValue = impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_dict_int_get"], {dict, key}, "dictget.i");
            }
            impl_->popArgTempCleanups(subBases);
            releaseOwnedKeyTemp();
            releaseOwnedRecvTempScalar(recvReleaseTag);
            return;
        }

        // D030 Phase 3.F: dispatch by checkTag to the typed runtime op so the value
        // crosses at its native type: FLOAT(2)->f64, STR/LIST/DICT/BYTES(1/5/6/7)->ptr, else i64.
        int64_t recvReleaseTag = -1;
        if (checkTag == TAG_FLOAT) {  // TAG_FLOAT
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_get_str_f64"], {dict, key}, "dictget.f");
            impl_->pendingDictCheckTag = -1;
            recvReleaseTag = 2;
        } else if (checkTag == 1 || checkTag == 5 || checkTag == 6 || checkTag == 7 ||
                   checkTag == TAG_CALLABLE) {  // TAG_CLOSURE - return the closure as a ptr so
                                      // the borrow-store increfs it (incref_callable)
            auto* tagVal = llvm::ConstantInt::get(impl_->i64Type, checkTag);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_get_str_ptr"], {dict, key, tagVal}, "dictget.p");
            impl_->pendingDictCheckTag = -1;
            if (checkTag == 5 && pendingListElem != Impl::kNoListElemCheck)
                impl_->builder->CreateCall(
                    impl_->runtimeFuncs["dragon_list_view_check"],
                    {impl_->lastValue,
                     llvm::ConstantInt::get(impl_->i64Type, pendingListElem)});
            retainElemThenReleaseRecv(checkTag);
        } else if (checkTag >= 0) {
            auto* tagVal = llvm::ConstantInt::get(impl_->i64Type, checkTag);
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_get_checked"], {dict, key, tagVal}, "dictget_chk");
            impl_->pendingDictCheckTag = -1;
            recvReleaseTag = checkTag;
        } else {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_dict_get"], {dict, key}, "dictget");
        }
        impl_->popArgTempCleanups(subBases);
        releaseOwnedKeyTemp();
        releaseOwnedRecvTempScalar(recvReleaseTag);
        return;
    }

    // Check for tuple subscript: t[0]
    bool isTuple = dynamic_cast<TupleExpr*>(node.object.get()) != nullptr;
    if (!isTuple) {
        if (auto* nameExpr = dynamic_cast<NameExpr*>(node.object.get())) {
            isTuple = impl_->lookupVarKind(nameExpr->name) == Impl::VarKind::Tuple;
        }
    }
    // Fallback: typechecker-propagated type. Handles chained subscripts.
    if (!isTuple && node.object->type &&
        node.object->type->kind() == Type::Kind::Tuple) {
        isTuple = true;
    }

    if (isTuple) {
        node.object->accept(*this);
        llvm::Value* tuplePtr = impl_->lastValue;
        std::vector<llvm::Value*> tupBases;
        impl_->pushTempCleanupByKind(
            tuplePtr, impl_->ownedTempDrainKind(node.object.get(), tuplePtr),
            tupBases);
        node.index->accept(*this);
        llvm::Value* tupleIdx = impl_->lastValue;
        if (tupleIdx->getType() == impl_->i1Type) {
            tupleIdx = impl_->builder->CreateZExt(tupleIdx, impl_->i64Type);
        }
        // tuple[Any, ...] element read: return the {tag, payload} box (BORROW,
        // tuple keeps +1) - the raw i64 path below would rebox a heap payload as TAG_INT (wrong tag).
        if (node.type && (node.type->kind() == Type::Kind::Any ||
                          node.type->kind() == Type::Kind::Union)) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_tuple_box_get"],
                {tuplePtr, tupleIdx}, "tupleget.box");
            impl_->popArgTempCleanups(tupBases);
            return;
        }
        llvm::Value* raw = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_tuple_get"], {tuplePtr, tupleIdx}, "tupleget");
        impl_->popArgTempCleanups(tupBases);
        // D030: convert i64 storage back to its native LLVM type, else `const matched:
        // dict = result[1]` stores i64 into a ptr alloca and storeWithRCOverwrite's incref silently no-ops -> shared ref with the tuple, double-free on cleanup.
        if (node.type) {
            auto k = node.type->kind();
            switch (k) {
                case Type::Kind::Str:
                case Type::Kind::Bytes:
                case Type::Kind::List:
                case Type::Kind::Dict:
                case Type::Kind::Set:
                case Type::Kind::Tuple:
                case Type::Kind::Instance:
                case Type::Kind::Ptr:
                    impl_->lastValue = impl_->builder->CreateIntToPtr(
                        raw, impl_->i8PtrType, "tupleget.ptr");
                    return;
                case Type::Kind::Float:
                    impl_->lastValue = impl_->builder->CreateBitCast(
                        raw, impl_->f64Type, "tupleget.f64");
                    return;
                case Type::Kind::Bool:
                    impl_->lastValue = impl_->builder->CreateICmpNE(
                        raw, llvm::ConstantInt::get(impl_->i64Type, 0), "tupleget.bool");
                    return;
                default:
                    break;
            }
        }
        impl_->lastValue = raw;
        return;
    }

    node.object->accept(*this);
    llvm::Value* obj = impl_->lastValue;
    Impl::VarKind subRecvDrain = Impl::VarKind::Other;
    if (obj->getType() != impl_->boxType)
        subRecvDrain = impl_->ownedTempDrainKind(node.object.get(), obj);
    std::vector<llvm::Value*> subBases;
    impl_->pushTempCleanupByKind(obj, subRecvDrain, subBases);
    node.index->accept(*this);
    llvm::Value* idx = impl_->lastValue;

    // Subscripting an Any-boxed value (`anyVal[i]`): `obj` is a 16-byte box, not a
    // pointer. Box the index too and let the runtime dispatch on the receiver's tag.
    if (obj->getType() == impl_->boxType) {
        llvm::Value* idxBox = impl_->boxNativeOperand(*this, node.index.get(), idx);
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_box_subscript"], {obj, idxBox},
            "box.subscript");
        // Chained subscript (`obj["b"][2]`): an OWNED box receiver is fully consumed
        // here - release its +1 AFTER the call. Borrowed receivers (dict_get_box etc, on isOwnedBoxResult's denylist) are never released - that would be a use-after-free.
        if (impl_->isOwnedBoxResult(obj))
            impl_->emitDecrefByKind(obj, Impl::VarKind::Union);
        // Same for an OWNED box INDEX (`d[keys["i"]]`): the runtime only reads it,
        // never retains it, so the subscript fully consumes the temporary (a boxNativeOperand-built index is never flagged owned, so it's untouched).
        if (impl_->isOwnedBoxResult(idxBox))
            impl_->emitDecrefByKind(idxBox, Impl::VarKind::Union);
        return;
    }

    // Ensure index is i64
    if (idx->getType() == impl_->i1Type) {
        idx = impl_->builder->CreateZExt(idx, impl_->i64Type);
    }

    if (!obj->getType()->isPointerTy()) {
        impl_->addError(
            "cannot subscript this expression: the receiver lowered to a "
            "non-pointer value, so the index would have silently produced 0",
            node.location());
        impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
        return;
    }

    // D030 §5: detect bytes BEFORE list - bytes-typed slots collapse onto
    // VarKind::List (generic-heap), so a bare VarKind check would misroute it through inline list-GEP.
    bool isBytes = impl_->exprIsBytes(node.object.get());

    // Determine list vs bytes vs string from VarKind or expression type
    bool isList = !isBytes && dynamic_cast<ListExpr*>(node.object.get()) != nullptr;
    if (!isList && !isBytes) {
        if (auto* nameExpr = dynamic_cast<NameExpr*>(node.object.get())) {
            auto vk = impl_->lookupVarKind(nameExpr->name);
            if (vk == Impl::VarKind::List) isList = true;
        } else if (auto* attrExpr = dynamic_cast<AttributeExpr*>(node.object.get())) {
            // self.field - look up field VarKind from classFieldKinds
            std::string className;
            if (auto* objName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                if (objName->name == "self" && !impl_->currentClassName.empty()) {
                    className = impl_->currentClassName;
                } else {
                    auto vit = impl_->varClassNames.find(objName->name);
                    if (vit != impl_->varClassNames.end()) className = vit->second;
                }
            }
            if (!className.empty()) {
                auto fkIt = impl_->classFieldKindsBySym.find(impl_->classSym(className));
                if (fkIt != impl_->classFieldKindsBySym.end()) {
                    auto fkIt2 = fkIt->second.find(attrExpr->attribute);
                    if (fkIt2 != fkIt->second.end()) {
                        if (fkIt2->second == Impl::VarKind::List) isList = true;
                    }
                }
            }
        }
    }
    // Fallback: typechecker-propagated type. Handles chained subscripts
    // (a[i][j], d["k"][i]) and other cases the VarKind heuristics miss.
    if (!isList && !isBytes && node.object->type &&
        node.object->type->kind() == Type::Kind::List) {
        isList = true;
    }

    if (isList) {
        // D039 Phase 4: list[Any] -> dragon_list_box_get (a box). Skips the inline
        // GEP fast path since DragonListBox is 16B/elem vs 8B/elem; the runtime helper also bounds-checks.
        bool elemIsAny = false;
        if (node.object->type) {
            if (auto* lt = dynamic_cast<ListType*>(node.object->type.get())) {
                if (lt->elementType && lt->elementType->kind() == Type::Kind::Any)
                    elemIsAny = true;
            }
        }
        if (!elemIsAny) {
            if (auto* nameExpr = dynamic_cast<NameExpr*>(node.object.get())) {
                auto it = impl_->varListElemKinds.find(nameExpr->name);
                if (it != impl_->varListElemKinds.end() &&
                    it->second == Type::Kind::Any)
                    elemIsAny = true;
            }
        }
        if (elemIsAny) {
            impl_->lastValue = impl_->builder->CreateCall(
                impl_->runtimeFuncs["dragon_list_box_get"], {obj, idx},
                "listget.box");
            impl_->popArgTempCleanups(subBases);
            return;
        }

        // Inline list access (direct GEP, not a dragon_list_get call). DragonList
        // layout: header(16B)|data*(8B)|size(8B)|cap(8B)|tag(1B); i64 offsets: data=2, size=3.
        auto* tbaaHdrTag = llvm::MDNode::get(*impl_->context,
            {impl_->tbaaListHeader, impl_->tbaaListHeader,
             llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(impl_->i64Type, 0))});
        auto* tbaaDataTag = llvm::MDNode::get(*impl_->context,
            {impl_->tbaaListData, impl_->tbaaListData,
             llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(impl_->i64Type, 0))});

        auto* dataGEP = impl_->builder->CreateGEP(impl_->i64Type, obj,
            llvm::ConstantInt::get(impl_->i64Type, 2), "list.data.gep");
        auto* dataLoad = impl_->builder->CreateLoad(impl_->i64Type, dataGEP, "list.data.raw");
        llvm::cast<llvm::Instruction>(dataLoad)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaHdrTag);
        auto* dataPtr = impl_->builder->CreateIntToPtr(dataLoad, impl_->i8PtrType, "list.data");
        auto* sizeGEP = impl_->builder->CreateGEP(impl_->i64Type, obj,
            llvm::ConstantInt::get(impl_->i64Type, 3), "list.size.gep");
        auto* sizeLoad = impl_->builder->CreateLoad(impl_->i64Type, sizeGEP, "list.size");
        llvm::cast<llvm::Instruction>(sizeLoad)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaHdrTag);
        auto* size = sizeLoad;

        // 6.12(B): skip the `idx + (idx<0 ? size : 0)` correction when the index is
        // provably non-negative (literal, len(), tracked counter) - saves 3 instructions/access; the bounds check below still catches the rest.
        llvm::Value* finalIdx;
        if (impl_->isExprDefinitelyNonNeg(node.index.get())) {
            finalIdx = idx;
        } else {
            auto* isNeg = impl_->builder->CreateICmpSLT(idx,
                llvm::ConstantInt::get(impl_->i64Type, 0), "idx.neg");
            auto* adjIdx = impl_->builder->CreateAdd(idx, size, "idx.adj");
            finalIdx = impl_->builder->CreateSelect(isNeg, adjIdx, idx, "idx.final");
        }

        // Bounds check (unsigned compare catches still-negative and >= size)
        auto* inBounds = impl_->builder->CreateICmpULT(finalIdx, size, "idx.ok");
        auto* func = impl_->currentFunction;
        auto* okBB = llvm::BasicBlock::Create(*impl_->context, "list.ok", func);
        auto* oobBB = llvm::BasicBlock::Create(*impl_->context, "list.oob", func);
        impl_->builder->CreateCondBr(inBounds, okBB, oobBB);

        // OOB: dragon_list_get raises IndexError (longjmp, never returns here)
        impl_->builder->SetInsertPoint(oobBB);
        impl_->builder->CreateCall(impl_->runtimeFuncs["dragon_list_get"], {obj, idx});
        impl_->builder->CreateUnreachable();

        // In-bounds: direct element load with TBAA. Stride matches the runtime's
        // elem_size - i8 for list[bool] (1MB not 8MB), i64 for everything else.
        impl_->builder->SetInsertPoint(okBB);
        impl_->popArgTempCleanups(subBases);
        // Resolve element kind (used for stride + unbox).
        Type::Kind elemKind = Type::Kind::Int;
        if (node.object->type) {
            if (auto* lt = dynamic_cast<ListType*>(node.object->type.get())) {
                if (lt->elementType) elemKind = lt->elementType->kind();
            }
        }
        if (elemKind == Type::Kind::Int) {
            if (auto* nameExpr = dynamic_cast<NameExpr*>(node.object.get())) {
                auto it = impl_->varListElemKinds.find(nameExpr->name);
                if (it != impl_->varListElemKinds.end())
                    elemKind = it->second;
            } else if (auto* attrExpr = dynamic_cast<AttributeExpr*>(node.object.get())) {
                // <class-instance>.<field>[i]: read the field's element kind from
                // classFieldListElemKinds (mirrors the for-loop logic in ForLoop.cpp).
                std::string className;
                if (auto* objName = dynamic_cast<NameExpr*>(attrExpr->object.get())) {
                    if (objName->name == "self" && !impl_->currentClassName.empty()) {
                        className = impl_->currentClassName;
                    } else {
                        auto vit = impl_->varClassNames.find(objName->name);
                        if (vit != impl_->varClassNames.end()) className = vit->second;
                    }
                }
                if (!className.empty()) {
                    auto cit = impl_->classFieldListElemKindsBySym.find(impl_->classSym(className));
                    if (cit != impl_->classFieldListElemKindsBySym.end()) {
                        auto fit = cit->second.find(attrExpr->attribute);
                        if (fit != cit->second.end()) elemKind = fit->second;
                    }
                }
            }
        }
        // D030 Phase 3.B: pick the GEP/load type matching the list variant: Bool->i8
        // (D028), Float->f64, heap kinds->ptr (DragonListPtr), else i64 (DragonList int*).
        bool isBoolElem  = (elemKind == Type::Kind::Bool);
        bool isFloatElem = (elemKind == Type::Kind::Float);
        bool isPtrElem   = (elemKind == Type::Kind::Str      ||
                            elemKind == Type::Kind::Bytes    ||
                            elemKind == Type::Kind::List     ||
                            elemKind == Type::Kind::Dict     ||
                            elemKind == Type::Kind::Tuple    ||
                            elemKind == Type::Kind::Set      ||
                            elemKind == Type::Kind::Instance ||
                            elemKind == Type::Kind::Function);  // list[Callable]
                            // stores closure ptrs in the DragonListPtr variant.
        auto* i8Ty = llvm::Type::getInt8Ty(*impl_->context);
        llvm::Type* strideTy;
        llvm::Type* loadTy;
        if (isBoolElem) {
            strideTy = i8Ty;
            loadTy = i8Ty;
        } else if (isFloatElem) {
            strideTy = impl_->f64Type;
            loadTy = impl_->f64Type;
        } else if (isPtrElem) {
            strideTy = impl_->i8PtrType;
            loadTy = impl_->i8PtrType;
        } else {
            strideTy = impl_->i64Type;
            loadTy = impl_->i64Type;
        }
        auto* elemGEP = impl_->builder->CreateGEP(strideTy, dataPtr,
            finalIdx, "list.elem.gep");
        auto* elemLoad = impl_->builder->CreateLoad(loadTy, elemGEP, "list.elem");
        llvm::cast<llvm::Instruction>(elemLoad)->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaDataTag);
        if (isBoolElem) {
            // D030 Phase 2: bool element loaded as i8, truncated to native i1 (was:
            // ZExt to i64). Consumers needing i64 widen via BinaryExpr's bool-promotion; print dispatches from i1 directly.
            impl_->lastValue = impl_->builder->CreateICmpNE(
                elemLoad, llvm::ConstantInt::get(elemLoad->getType(), 0), "list.elem.b");
        } else {
            // Float / Ptr / Int: load is already at the native type - no
            // bitcast / IntToPtr needed.
            impl_->lastValue = elemLoad;
        }
        // An OWNED receiver temp (`make()[0]`) is released only for a PROVABLY
        // SCALAR element read (audit 1.7) - a ptr element is borrowed FROM the receiver, so releasing here before the consumer takes its ref would be a use-after-free.
        bool elemProvablyScalar = false;
        if (node.object->type) {
            if (auto* lt = dynamic_cast<ListType*>(node.object->type.get())) {
                if (lt->elementType) {
                    auto ek = lt->elementType->kind();
                    elemProvablyScalar = (ek == Type::Kind::Int ||
                                          ek == Type::Kind::Float ||
                                          ek == Type::Kind::Bool);
                }
            }
        }
        if (!isPtrElem && elemProvablyScalar &&
            subRecvDrain != Impl::VarKind::Other)
            impl_->emitDecrefByKind(obj, subRecvDrain);
    } else if (isBytes) {
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_bytes_get"], {obj, idx}, "bytesget");
        impl_->popArgTempCleanups(subBases);
        // bytes[i] copies out an int - an owned bytes receiver temp is done.
        if (subRecvDrain != Impl::VarKind::Other)
            impl_->emitDecrefByKind(obj, subRecvDrain);
    } else {
        impl_->lastValue = impl_->builder->CreateCall(
            impl_->runtimeFuncs["dragon_str_index"], {obj, idx}, "strget");
        impl_->popArgTempCleanups(subBases);
        // s[i] mallocs a FRESH 1-char string (see isBorrowedHeapExpr) - the
        // owned str receiver temp (`("Z" + p)[0]`) is fully consumed here.
        if (subRecvDrain != Impl::VarKind::Other)
            impl_->emitDecrefByKind(obj, subRecvDrain);
    }
}
void CodeGen::visit(SliceExpr& node) {
    impl_->addError(
        "internal error: slice expression reached codegen outside a "
        "subscript",
        node.location());
    impl_->lastValue = llvm::ConstantInt::get(impl_->i64Type, 0);
}
} // namespace dragon
