#include "../CodeGenImpl.h"
#include "dragon/Privacy.h"

namespace dragon {

void CodeGen::Impl::init() {
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("dragon_module", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    if (options.targetTriple.empty()) {
        options.targetTriple = llvm::sys::getDefaultTargetTriple();
    }
    module->setTargetTriple(llvm::Triple(options.targetTriple));

    {
        std::string tlErr;
        const llvm::Target* tgt = llvm::TargetRegistry::lookupTarget(
            module->getTargetTriple(), tlErr);
        if (tgt) {
            llvm::TargetOptions tOpts;
            std::unique_ptr<llvm::TargetMachine> tm(tgt->createTargetMachine(
                module->getTargetTriple(), "generic", "", tOpts,
                std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_)));
            if (tm) module->setDataLayout(tm->createDataLayout());
        }
    }

    i64Type = llvm::Type::getInt64Ty(*context);
    llvm::Triple triple(options.targetTriple);
    intcType = triple.isArch16Bit()
        ? llvm::Type::getInt16Ty(*context)
        : llvm::Type::getInt32Ty(*context);
    f64Type = llvm::Type::getDoubleTy(*context);
    i1Type = llvm::Type::getInt1Ty(*context);
    i8PtrType = llvm::PointerType::getUnqual(*context);
    voidType = llvm::Type::getVoidTy(*context);

    boxType = llvm::StructType::create(
        *context, {i64Type, i64Type}, "dragon.box");

    tbaaRoot = llvm::MDNode::get(*context, {
        llvm::MDString::get(*context, "Dragon TBAA")});
    tbaaListHeader = llvm::MDNode::get(*context, {
        llvm::MDString::get(*context, "list header"), tbaaRoot,
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Type, 0))});
    tbaaListData = llvm::MDNode::get(*context, {
        llvm::MDString::get(*context, "list data"), tbaaRoot,
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Type, 0))});

    scopes.push_back({});
}

void CodeGen::Impl::declareRuntimeFunctions() {
    getOrDeclareRuntime("dragon_print_int",
        llvm::FunctionType::get(voidType, {i64Type}, false));
    getOrDeclareRuntime("dragon_print_float",
        llvm::FunctionType::get(voidType, {f64Type}, false));
    getOrDeclareRuntime("dragon_print_str",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_bool",
        llvm::FunctionType::get(voidType, {i64Type}, false));
    getOrDeclareRuntime("dragon_print_none",
        llvm::FunctionType::get(voidType, {}, false));
    getOrDeclareRuntime("dragon_print_newline",
        llvm::FunctionType::get(voidType, {}, false));
    getOrDeclareRuntime("dragon_print_space",
        llvm::FunctionType::get(voidType, {}, false));
    getOrDeclareRuntime("dragon_print_int_raw",
        llvm::FunctionType::get(voidType, {i64Type}, false));
    getOrDeclareRuntime("dragon_print_float_raw",
        llvm::FunctionType::get(voidType, {f64Type}, false));
    getOrDeclareRuntime("dragon_print_str_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_bool_raw",
        llvm::FunctionType::get(voidType, {i64Type}, false));
    getOrDeclareRuntime("dragon_print_none_raw",
        llvm::FunctionType::get(voidType, {}, false));
    getOrDeclareRuntime("dragon_print_box_raw",
        llvm::FunctionType::get(voidType, {boxType}, false));
    getOrDeclareRuntime("dragon_print_tagged_raw",
        llvm::FunctionType::get(voidType, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_print_list_int_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_list_str_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_list_float_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_list_bool_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_list_box_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_dict_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_dict_int_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_list_nested_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_dict_nested_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_dict_int_nested_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_tuple_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_set_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_bytes_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_input",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_concat",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_append_inplace",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_eq",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_cmp",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_contains",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_assert",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_assert_no_msg",
        llvm::FunctionType::get(voidType, {i64Type}, false));
    getOrDeclareRuntime("dragon_pow_int",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_pow_int_checked",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_floordiv_int",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_mod_int",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_abs_int",
        llvm::FunctionType::get(i64Type, {i64Type}, false));
    getOrDeclareRuntime("dragon_abs_float",
        llvm::FunctionType::get(f64Type, {f64Type}, false));
    getOrDeclareRuntime("dragon_int_to_str",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_float_to_str",
        llvm::FunctionType::get(i8PtrType, {f64Type}, false));
    getOrDeclareRuntime("dragon_float_format",
        llvm::FunctionType::get(i8PtrType, {f64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_int_format",
        llvm::FunctionType::get(i8PtrType, {i64Type, i8PtrType}, false));

    getOrDeclareRuntime("dragon_list_new",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_list_new_tagged",
        llvm::FunctionType::get(i8PtrType, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_list_repeat",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_append",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));

    getOrDeclareRuntime("dragon_list_new_f64",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_list_get_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_set_f64",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, f64Type}, false));
    getOrDeclareRuntime("dragon_list_append_f64",
        llvm::FunctionType::get(voidType, {i8PtrType, f64Type}, false));
    getOrDeclareRuntime("dragon_list_new_ptr",
        llvm::FunctionType::get(i8PtrType, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_list_get_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_set_ptr",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_list_append_ptr",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_join_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_list_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_list_int",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_list_str",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_list_float",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_list_bool",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_list_insert",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_list_remove",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_pop_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_delitem",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_box_delitem",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_clear",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_list_extend",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_list_index",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_count",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_contains",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_to_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_list_box_to_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_to_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_int_to_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_to_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_tuple_to_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_list_sort",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_list_reverse",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_list_copy",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_str_index",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));

    getOrDeclareRuntime("dragon_str_slice",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_list_slice",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i64Type, i64Type}, false));

    getOrDeclareRuntime("dragon_bool_to_str",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));

    getOrDeclareRuntime("dragon_str_repeat",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));

    getOrDeclareRuntime("dragon_dict_new",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_dict_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_set_tagged",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_str_iaug_i64",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_get_tag",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_get_checked",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_get_box",
        llvm::FunctionType::get(boxType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_box",
        llvm::FunctionType::get(voidType, {boxType}, false));

    getOrDeclareRuntime("dragon_list_box_new",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_list_box_get",
        llvm::FunctionType::get(boxType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_view_check",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_box_len",
        llvm::FunctionType::get(i64Type, {boxType}, false));
    getOrDeclareRuntime("dragon_list_box_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_list_box_append",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_list_box_pop",
        llvm::FunctionType::get(boxType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_box_remove",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_list_box_insert",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_list_box_destroy",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_list_box",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_box_eq",
        llvm::FunctionType::get(i64Type, {boxType, boxType}, false));
    getOrDeclareRuntime("dragon_box_binop",
        llvm::FunctionType::get(boxType, {boxType, boxType, i64Type}, false));
    getOrDeclareRuntime("dragon_box_cmp",
        llvm::FunctionType::get(i64Type, {boxType, boxType, i64Type}, false));
    getOrDeclareRuntime("dragon_box_to_str",
        llvm::FunctionType::get(i8PtrType, {boxType}, false));
    getOrDeclareRuntime("dragon_box_subscript",
        llvm::FunctionType::get(boxType, {boxType, boxType}, false));
    getOrDeclareRuntime("dragon_box_decref",
        llvm::FunctionType::get(voidType, {boxType}, false));

    getOrDeclareRuntime("dragon_list_eq",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_list_cmp",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_eq",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_int_eq",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));

    getOrDeclareRuntime("dragon_dict_values_box",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_dict_get_str_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_set_str_f64",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType, f64Type}, false));
    getOrDeclareRuntime("dragon_dict_get_str_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_set_str_ptr",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType, i8PtrType, i64Type}, false));

    getOrDeclareRuntime("dragon_dict_mark_float_keys",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_dict_int_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_set_tagged",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_set_f64",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, f64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_set_str",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_int_set_ptr",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_get_tag",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_get_checked",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_get_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_get_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_get_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_get_default",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_has_key",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_del",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_pop_default",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_setdefault",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_keys",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_dict_int",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_tagged",
        llvm::FunctionType::get(voidType, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_has_key",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_reject_unknown_keys",
        llvm::FunctionType::get(voidType,
            {i8PtrType, i8PtrType, i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_get_default",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_get_str_default",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_get_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_get_ptr_default",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_int_get_owned",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_get_owned_default",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_setdefault_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_int_setdefault_owned",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_keys",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_dict",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_values",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_items",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_del",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_pop_default",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_popitem",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_fromkeys",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_clear",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_update",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_setdefault",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_dict_copy",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_copy_excluding",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type}, false));

    getOrDeclareRuntime("dragon_tuple_new",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_tuple_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_tuple_box_get",
        llvm::FunctionType::get(boxType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_tuple_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_tuple_set_tagged",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_tuple_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_tuple",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_set_new",
        llvm::FunctionType::get(i8PtrType, {}, false));
    getOrDeclareRuntime("dragon_set_new_tagged",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_set_from_list",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_adopt_tag",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_set_add",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_set_contains",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_set_union",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_intersection",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_difference",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_symmetric_difference",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_remove",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_set_discard",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_set_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_clear",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_copy",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_union",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_intersection",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_difference",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_symmetric_difference",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_issubset",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_issuperset",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_isdisjoint",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_set_update",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_set",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_deque_new",
        llvm::FunctionType::get(i8PtrType, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_deque_append",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_deque_appendleft",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_deque_popleft",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_deque_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_deque_popleft_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_deque_pop_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_deque_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_deque_contains",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_deque_from_list",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_deque_to_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_deque_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_deque_destroy",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_exc_push_frame",
        llvm::FunctionType::get(i8PtrType, {}, false));
    getOrDeclareRuntime("dragon_exc_pop_frame",
        llvm::FunctionType::get(voidType, {}, false));
    getOrDeclareRuntime("dragon_exc_get_type",
        llvm::FunctionType::get(i64Type, {}, false));
    getOrDeclareRuntime("dragon_exc_get_msg",
        llvm::FunctionType::get(i8PtrType, {}, false));
    getOrDeclareRuntime("dragon_raise_exc",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_raise_exc_cstr",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_raise_exc_obj",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_raise_exc_consume",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_raise_exc_obj_consume",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_exc_bind_msg",
        llvm::FunctionType::get(i8PtrType, {}, false));
    getOrDeclareRuntime("dragon_exc_bind_obj",
        llvm::FunctionType::get(i8PtrType, {}, false));
    getOrDeclareRuntime("dragon_exc_retain_obj",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    auto* i32Ty = llvm::Type::getInt32Ty(*context);
    getOrDeclareRuntime("dragon_cleanup_push",
        llvm::FunctionType::get(i32Ty, {i64Type, i32Ty, i32Ty}, false));
    getOrDeclareRuntime("dragon_cleanup_update",
        llvm::FunctionType::get(voidType, {i32Ty, i64Type, i32Ty}, false));
    getOrDeclareRuntime("dragon_cleanup_depth",
        llvm::FunctionType::get(i32Ty, {}, false));
    getOrDeclareRuntime("dragon_cleanup_reset",
        llvm::FunctionType::get(voidType, {i32Ty}, false));
    getOrDeclareRuntime("dragon_exc_cleanup_unwind",
        llvm::FunctionType::get(voidType, {}, false));
    getOrDeclareRuntime("dragon_exc_get_obj",
        llvm::FunctionType::get(i8PtrType, {}, false));
    getOrDeclareRuntime("dragon_exc_register",
        llvm::FunctionType::get(voidType, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_exc_matches",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_vthread_log_uncaught",
        llvm::FunctionType::get(voidType, {}, false));
    {
        auto* setjmpType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(*context), {i8PtrType}, false);
        auto* setjmpFunc = getOrDeclareRuntime("setjmp", setjmpType);
        setjmpFunc->addFnAttr(llvm::Attribute::ReturnsTwice);
    }

    getOrDeclareRuntime("dragon_min_int",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_max_int",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_min_float",
        llvm::FunctionType::get(f64Type, {f64Type, f64Type}, false));
    getOrDeclareRuntime("dragon_max_float",
        llvm::FunctionType::get(f64Type, {f64Type, f64Type}, false));
    getOrDeclareRuntime("dragon_min_list",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_max_list",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_sum_list",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_min_list_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_max_list_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_min_list_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_max_list_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_sum_list_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_any_list",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_all_list",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_enumerate",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_zip",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_sorted",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_sorted_ex",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_sort_ex",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_list_concat",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_reversed",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_hash_int",
        llvm::FunctionType::get(i64Type, {i64Type}, false));
    getOrDeclareRuntime("dragon_hash_str",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_id",
        llvm::FunctionType::get(i64Type, {i64Type}, false));

    getOrDeclareRuntime("dragon_ord",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_chr",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_round_int",
        llvm::FunctionType::get(i64Type, {f64Type}, false));
    getOrDeclareRuntime("dragon_pow_float",
        llvm::FunctionType::get(f64Type, {f64Type, f64Type}, false));
    getOrDeclareRuntime("dragon_divmod",
        llvm::FunctionType::get(i8PtrType, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_hex",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_oct",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_bin",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_repr_int",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_repr_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_repr_float",
        llvm::FunctionType::get(i8PtrType, {f64Type}, false));
    getOrDeclareRuntime("dragon_repr_bool",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));

    getOrDeclareRuntime("dragon_file_open",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_file_close",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_file_read",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_file_readline",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_bytes_from_literal",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_bytes_from_list",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_print_bytes",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_concat",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_repeat",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_bytes_eq",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_cmp",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_bytes_slice",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_bytes_contains",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_bytes_contains_bytes",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_decode",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_encode",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_find",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_rfind",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_count",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_index_of",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_rindex",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_startswith",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_endswith",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_replace",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_upper",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_lower",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_strip",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_lstrip",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_rstrip",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_split",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_join",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_isdigit",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_isalpha",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_isalnum",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_isspace",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_hex",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_bytes_fromhex",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_thread_fire",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_thread_join",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_vthread_spawn_typed",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_vthread_set_result",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("mco_get_user_data",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("free",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_vthread_join",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_vthread_detach",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_vthread_is_alive",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_vthread_sleep",
        llvm::FunctionType::get(voidType, {i64Type}, false));
    getOrDeclareRuntime("dragon_vthread_yield",
        llvm::FunctionType::get(voidType, {}, false));

    getOrDeclareRuntime("dragon_generator_create_typed",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_generator_set_exhausted",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_generator_set_raised",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_generator_yield",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_generator_next",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_generator_destroy",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_generator_abandon",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_osthread_new",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_osthread_start",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_osthread_join",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_osthread_is_alive",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_lock_new",
        llvm::FunctionType::get(i8PtrType, {}, false));
    getOrDeclareRuntime("dragon_lock_acquire",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_lock_acquire_ex",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, f64Type}, false));
    getOrDeclareRuntime("dragon_lock_try_acquire",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_lock_release",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_lock_destroy",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_del_assert_unique",
        llvm::FunctionType::get(voidType,
            {i8PtrType, i64Type, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_del_assert_unique_box",
        llvm::FunctionType::get(voidType,
            {i64Type, i64Type, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_synclist_new",
        llvm::FunctionType::get(i8PtrType, {}, false));
    getOrDeclareRuntime("dragon_synclist_append",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_synclist_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_synclist_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_synclist_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_synclist_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_synclist_clear",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_synclist_extend",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_synclist_remove",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_synclist_insert",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_synclist_index",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_synclist_count",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_synclist_sort",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_synclist_reverse",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_synclist_copy",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_synclist_destroy",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_new",
        llvm::FunctionType::get(i8PtrType, {}, false));
    getOrDeclareRuntime("dragon_syncdict_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_syncdict_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_get_default",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_syncdict_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_has_key",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_keys",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_values",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_items",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_pop_default",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_syncdict_clear",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_update",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_setdefault",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_syncdict_copy",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_syncdict_destroy",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_incref",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_decref",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_incref_str",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_decref_str",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_make_immortal",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_incref_callable",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_decref_callable",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_string_dup",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_retain",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_exc_msg_preserve",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_intern",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));

    getOrDeclareRuntime("dragon_incref_atomic",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_decref_atomic",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_incref_str_atomic",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_decref_str_atomic",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_mark_shared_deep",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_mark_shared",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_mark_shared_str",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_mark_shared_worklist_push",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_mark_shared_callable",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_class_register_mark_shared",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_mark_shared_boxed",
        llvm::FunctionType::get(voidType, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_mark_shared_cell",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));

    getOrDeclareRuntime("dragon_class_register_dealloc",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_class_register_traverse",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_class_register_clear",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_gc_track",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_gc_untrack",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_gc_collect",
        llvm::FunctionType::get(i64Type, {}, false));
    getOrDeclareRuntime("dragon_gc_set_threshold",
        llvm::FunctionType::get(voidType, {i64Type}, false));

    getOrDeclareRuntime("dragon_class_descriptor_create",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, i64Type, i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_class_descriptor_get_name",
        llvm::FunctionType::get(i64Type, {i64Type}, false));
    getOrDeclareRuntime("dragon_class_descriptor_get_doc",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    getOrDeclareRuntime("dragon_instance_get_doc",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));

    getOrDeclareRuntime("dragon_class_descriptor_set_fields",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType, i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_hasattr",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_getattr",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_getattr_default",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType, i64Type}, false));

    getOrDeclareRuntime("dragon_class_descriptor_set_methods",
        llvm::FunctionType::get(voidType,
            {i64Type, i8PtrType, i8PtrType, i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_class_find_method",
        llvm::FunctionType::get(i8PtrType, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_class_find_method_kind",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dir",
        llvm::FunctionType::get(i8PtrType, {i64Type, i64Type}, false));
    getOrDeclareRuntime("dragon_class_descriptor_set_method_bound_thunks",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_class_find_method_bound",
        llvm::FunctionType::get(i8PtrType, {i64Type, i8PtrType}, false));

    getOrDeclareRuntime("dragon_env_alloc",
        llvm::FunctionType::get(i8PtrType,
            {i64Type, i8PtrType, llvm::Type::getInt32Ty(*context)}, false));
    getOrDeclareRuntime("dragon_closure_create",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));

    getOrDeclareRuntime("dragon_cell_alloc",
        llvm::FunctionType::get(i8PtrType,
            {i64Type, llvm::Type::getInt32Ty(*context),
             llvm::Type::getInt32Ty(*context)}, false));
    getOrDeclareRuntime("dragon_cell_get",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_cell_set",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));

    getOrDeclareRuntime("dragon_template_escape_html",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_template_escape_sql",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_template_escape_url",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
}

void CodeGen::Impl::forwardDeclareFunctions(dragon::Module& mod) {
    for (auto& stmt : mod.body) {
        if (auto* func = dynamic_cast<FunctionDecl*>(stmt.get())) {
            if (!func->typeParams.empty()) continue;
            const std::string externLinkName =
                func->externSymbol.empty() ? func->name : func->externSymbol;
            const std::string llvmName = func->isExtern
                ? userFuncName(externLinkName)
                : mangleFunc(currentModuleName, func->name);
            if (module->getFunction(llvmName)) {
                if (func->isExtern && !func->externSymbol.empty())
                    importedFuncAliasesByModule[currentModuleName][func->name] = llvmName;
                if (options.gcMode == GCMode::RC && func->isExtern) {
                    if (!funcParamKinds.count(llvmName)) {
                        std::vector<VarKind> pkinds;
                        for (auto& p : func->params)
                            pkinds.push_back(typeExprToKind(p.type.get()));
                        funcParamKinds[llvmName] = std::move(pkinds);
                    }
                    externFuncNames.insert(llvmName);
                    bool ptrReturn = false;
                    if (auto* rn = dynamic_cast<NamedTypeExpr*>(func->returnType.get()))
                        ptrReturn = (rn->name == "ptr");
                    if (!ptrReturn) externDrainableFuncs.insert(llvmName);
                }
                continue;
            }
            std::vector<llvm::Type*> paramTypes;
            std::vector<bool> tagMask;
            VarArgInfo vaInfo;
            bool seenVarArg = false;
            for (auto& p : func->params) {
                if (p.isVarArg) {
                    seenVarArg = true;
                    if (!p.name.empty()) {
                        vaInfo.hasVarArg = true;
                        vaInfo.varArgName = p.name;
                        if (p.type) {
                            Type::Kind tk =
                                elemVarKindToTypeKind(typeExprToKind(p.type.get()));
                            vaInfo.varArgElemTag = typeKindToElemTag(tk);
                            vaInfo.varArgElemIsAny = (tk == Type::Kind::Any);
                        }
                        paramTypes.push_back(i8PtrType);
                        tagMask.push_back(false);
                    }
                    continue;
                }
                if (p.isKwArg) {
                    vaInfo.hasKwArg = true;
                    vaInfo.kwArgName = p.name;
                    paramTypes.push_back(i8PtrType);
                    tagMask.push_back(false);
                    continue;
                }
                if (!seenVarArg)
                    vaInfo.numRegularParams++;
                paramTypes.push_back(typeExprToLLVM(p.type.get()));
                tagMask.push_back(false);
            }
            if (vaInfo.hasVarArg || vaInfo.hasKwArg)
                funcVarArgInfo[llvmName] = vaInfo;
            llvm::Type* retType;
            if (func->isExtern && !func->returnType) {
                retType = voidType;
            } else if (func->isAsync) {
                retType = i8PtrType;
            } else if (containsYield(func->body)) {
                retType = i8PtrType;
                generatorFunctions.insert(llvmName);
            } else if (!func->returnType) {
                retType = unannotatedReturnType(func->body);
            } else {
                retType = typeExprToLLVM(func->returnType.get());
            }
            if (functionReturnsClosure(*func))
                funcReturnsClosure.insert(llvmName);
            {
                std::vector<bool> cp;
                cp.reserve(func->params.size());
                for (auto& p : func->params)
                    cp.push_back(dynamic_cast<CallableTypeExpr*>(p.type.get()) != nullptr);
                funcCallableParam[llvmName] = std::move(cp);
            }
            auto* funcType = llvm::FunctionType::get(retType, paramTypes, false);
            auto* llvmFunc = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                   llvmName, module.get());
            if (options.gcMode == GCMode::RC) {
                std::vector<VarKind> pkinds;
                std::vector<bool> powns;
                for (auto& p : func->params) {
                    pkinds.push_back(typeExprToKind(p.type.get()));
                    powns.push_back(p.isOwn);
                }
                funcParamKinds[llvmName] = std::move(pkinds);
                funcParamOwns[llvmName] = std::move(powns);
                if (func->isExtern) {
                    externFuncNames.insert(llvmName);
                    bool ptrReturn = false;
                    if (auto* rn = dynamic_cast<NamedTypeExpr*>(func->returnType.get()))
                        ptrReturn = (rn->name == "ptr");
                    if (!ptrReturn) externDrainableFuncs.insert(llvmName);
                }
            }
            {
                std::vector<Expr*> defaults;
                for (auto& p : func->params)
                    defaults.push_back(p.defaultValue.get());
                funcParamDefaults[llvmName] = std::move(defaults);
                funcDefiningModule[llvmName] = currentModuleName;
            }
            {
                std::vector<std::string> names;
                for (auto& p : func->params)
                    names.push_back(p.name);
                funcParamNames[llvmName] = std::move(names);
            }
            if (auto* retNamed = dynamic_cast<NamedTypeExpr*>(func->returnType.get())) {
                if (classNames.count(retNamed->name))
                    funcReturnClassNames[llvmName] = retNamed->name;
            } else if (func->returnType) {
                std::string gn = genericInstanceClassName(func->returnType.get());
                if (!gn.empty()) funcReturnClassNames[llvmName] = gn;
            }
            if (func->isExtern && !func->externLib.empty()) {
                externLibs.insert(func->externLib);
            }
            if (func->isExtern && externLinkName.substr(0, 7) == "sqlite3") {
                needsSqlite3 = true;
            }
            if (func->isExtern && externLinkName.substr(0, 5) == "pcre2") {
                needsPcre2 = true;
            }
            if (func->isExtern &&
                (externLinkName.substr(0, 10) == "dragon_tls" ||
                 externLinkName.substr(0, 10) == "dragon_sha" ||
                 externLinkName.substr(0, 10) == "dragon_md5" ||
                 externLinkName.substr(0, 11) == "dragon_hmac")) {
                needsMbedtls = true;
            }
            if (func->isExtern && externLinkName.substr(0, 11) == "dragon_zlib") {
                needsZ = true;
            }
            if (func->isExtern && externLinkName.substr(0, 11) == "dragon_zstd") {
                needsZstd = true;
            }
            if (func->isExtern &&
                externLinkName.substr(0, 14) == "dragon_webview") {
                needsWebview = true;
            }
            if (func->isExtern && !func->externSymbol.empty()) {
                importedFuncAliasesByModule[currentModuleName][func->name] = llvmName;
            }
            (void)llvmFunc;
        }
    }
}

void CodeGen::Impl::forwardDeclareClasses(dragon::Module& mod) {
    for (auto& stmt : mod.body) {
        if (auto* classDecl = dynamic_cast<ClassDecl*>(stmt.get())) {
            if (!classDecl->typeParams.empty()) continue;
            synthesizeEnumMethods(*classDecl);
            synthesizeDataclassMethods(*classDecl);
        }
    }
    for (auto& stmt : mod.body) {
        if (auto* classDecl = dynamic_cast<ClassDecl*>(stmt.get())) {
            if (!classDecl->typeParams.empty()) continue;
            bool isTD = false;
            for (auto& base : classDecl->bases) {
                if (auto* bn = dynamic_cast<NameExpr*>(base.get()))
                    if (bn->name == "TypedDict") isTD = true;
            }
            if (isTD) {
                std::string tdSym = mangleClass(
                    classDecl->genericHomeModule.empty() ? currentModuleName
                                                         : classDecl->genericHomeModule,
                    classDecl->name);
                typedDictClassesBySym.insert(tdSym);
                classOwningModule[classDecl->name] =
                    classDecl->genericHomeModule.empty()
                        ? currentModuleName : classDecl->genericHomeModule;
                for (auto& bs : classDecl->body) {
                    if (auto* ann = dynamic_cast<AnnAssignStmt*>(bs.get())) {
                        if (auto* fn = dynamic_cast<NameExpr*>(ann->target.get())) {
                            typedDictFieldKindsBySym[tdSym][fn->name] =
                                typeExprToTypeKind(ann->annotation.get());
                        }
                    }
                }
            } else {
                classNames.insert(classDecl->name);
                classOwningModule[classDecl->name] =
                    classDecl->genericHomeModule.empty()
                        ? currentModuleName
                        : classDecl->genericHomeModule;
            }
        }
    }
    for (auto& stmt : mod.body) {
        if (auto* classDecl = dynamic_cast<ClassDecl*>(stmt.get())) {
            if (!classDecl->typeParams.empty()) continue;
            const std::string csym = mangleClass(
                classDecl->genericHomeModule.empty() ? currentModuleName
                                                     : classDecl->genericHomeModule,
                classDecl->name);
            if (typedDictClassesBySym.count(csym)) continue;

            std::string _savedMod = currentModuleName;
            struct RM { std::string* s; std::string v; bool a; ~RM(){ if (a) *s = v; } }
                _rm{&currentModuleName, _savedMod,
                    !classDecl->genericHomeModule.empty()};
            if (!classDecl->genericHomeModule.empty())
                currentModuleName = classDecl->genericHomeModule;

            if (!classDecl->bases.empty()) {
                std::string baseBareName;
                if (auto* baseName = dynamic_cast<NameExpr*>(classDecl->bases[0].get())) {
                    baseBareName = baseName->name;
                } else if (auto* baseAttr =
                               dynamic_cast<AttributeExpr*>(classDecl->bases[0].get())) {
                    baseBareName = baseAttr->attribute;
                }
                if (!baseBareName.empty()) {
                    classParentNamesBySym[csym] = classSymPrefix(baseBareName);

                    if (isExcType(baseBareName)) {
                        int64_t code = userExcNextCode++;
                        userExcCodesBySym[csym] = code;
                        userExcParentCodes[code] = excTypeCode(baseBareName);
                    }
                }
            }

            std::vector<FunctionDecl*> initDecls;
            for (auto& classStmt : classDecl->body) {
                if (auto* fd = dynamic_cast<FunctionDecl*>(classStmt.get())) {
                    if (fd->name == "__init__") initDecls.push_back(fd);
                }
            }

            size_t ctorCount = initDecls.size();

            const std::string clsSym = mangleClass(currentModuleName, classDecl->name);

            if (ctorCount == 0) {
                std::string initName = clsSym + "___init__";
                if (!module->getFunction(initName)) {
                    auto* initFuncType = llvm::FunctionType::get(
                        voidType, {i8PtrType}, false);
                    llvm::Function::Create(initFuncType, llvm::Function::InternalLinkage,
                                           initName, module.get());
                }
                std::string newName = clsSym + "_new";
                if (!module->getFunction(newName)) {
                    auto* newFuncType = llvm::FunctionType::get(i8PtrType, {}, false);
                    llvm::Function::Create(newFuncType, llvm::Function::InternalLinkage,
                                           newName, module.get());
                }
                if (options.gcMode == GCMode::RC) {
                    funcParamKinds[newName] = {};
                    funcParamOwns[newName] = {};
                }
            } else if (ctorCount == 1) {
                FunctionDecl* initDecl = initDecls[0];

                std::vector<llvm::Type*> initParamTypes = {i8PtrType};
                std::vector<llvm::Type*> newParamTypes;
                size_t paramStart = initDecl->hasImplicitSelf ? 0 : 1;
                for (size_t i = paramStart; i < initDecl->params.size(); ++i) {
                    llvm::Type* pt = typeExprToLLVM(initDecl->params[i].type.get());
                    initParamTypes.push_back(pt);
                    newParamTypes.push_back(pt);
                }

                std::string initName = clsSym + "___init__";
                if (!module->getFunction(initName)) {
                    auto* initFuncType = llvm::FunctionType::get(voidType, initParamTypes, false);
                    llvm::Function::Create(initFuncType, llvm::Function::InternalLinkage,
                                           initName, module.get());
                }

                std::string newName = clsSym + "_new";
                if (!module->getFunction(newName)) {
                    auto* newFuncType = llvm::FunctionType::get(i8PtrType, newParamTypes, false);
                    llvm::Function::Create(newFuncType, llvm::Function::InternalLinkage,
                                           newName, module.get());
                }

                // Registers ctor VarKinds/own flags before any method lowers (see
                // fire-own-fwdref-hang.md) - else a forward-referenced ctor drains an owned param (UAF).
                if (options.gcMode == GCMode::RC) {
                    std::vector<VarKind> ck;
                    std::vector<bool> cowns;
                    for (size_t i = paramStart; i < initDecl->params.size(); ++i) {
                        ck.push_back(typeExprToKind(initDecl->params[i].type.get()));
                        cowns.push_back(initDecl->params[i].isOwn);
                    }
                    funcParamKinds[newName] = std::move(ck);
                    funcParamOwns[newName] = std::move(cowns);
                }

                {
                    std::vector<Expr*> defaults;
                    for (size_t i = paramStart; i < initDecl->params.size(); ++i) {
                        defaults.push_back(initDecl->params[i].defaultValue.get());
                    }
                    funcParamDefaults[newName] = std::move(defaults);
                    funcDefiningModule[newName] = currentModuleName;
                }
                {
                    std::vector<std::string> names;
                    for (size_t i = paramStart; i < initDecl->params.size(); ++i) {
                        names.push_back(initDecl->params[i].name);
                    }
                    funcParamNames[newName] = std::move(names);
                }
            } else {
                classCtorCountBySym[csym] = ctorCount;
                auto& arityVec = classCtorAritiesBySym[csym];
                arityVec.clear();

                for (size_t ci = 0; ci < ctorCount; ++ci) {
                    FunctionDecl* fd = initDecls[ci];
                    int ctorIdx = fd->constructorIndex >= 0 ? fd->constructorIndex : (int)ci;

                    size_t paramStart = fd->hasImplicitSelf ? 0 : 1;
                    size_t arity = fd->params.size() - paramStart;
                    arityVec.push_back({arity, ctorIdx});

                    std::vector<llvm::Type*> initParamTypes = {i8PtrType};
                    std::vector<llvm::Type*> newParamTypes;
                    for (size_t i = paramStart; i < fd->params.size(); ++i) {
                        llvm::Type* pt = typeExprToLLVM(fd->params[i].type.get());
                        initParamTypes.push_back(pt);
                        newParamTypes.push_back(pt);
                    }

                    std::string initName = clsSym + "___init___" + std::to_string(ctorIdx);
                    if (!module->getFunction(initName)) {
                        auto* initFuncType = llvm::FunctionType::get(voidType, initParamTypes, false);
                        llvm::Function::Create(initFuncType, llvm::Function::InternalLinkage,
                                               initName, module.get());
                    }

                    std::string newName = clsSym + "_new_" + std::to_string(ctorIdx);
                    if (!module->getFunction(newName)) {
                        auto* newFuncType = llvm::FunctionType::get(i8PtrType, newParamTypes, false);
                        llvm::Function::Create(newFuncType, llvm::Function::InternalLinkage,
                                               newName, module.get());
                    }

                    if (options.gcMode == GCMode::RC) {
                        std::vector<VarKind> ck;
                        std::vector<bool> cowns;
                        for (size_t i = paramStart; i < fd->params.size(); ++i) {
                            ck.push_back(typeExprToKind(fd->params[i].type.get()));
                            cowns.push_back(fd->params[i].isOwn);
                        }
                        funcParamKinds[newName] = std::move(ck);
                        funcParamOwns[newName] = std::move(cowns);
                    }
                }
            }

            for (auto& classStmt : classDecl->body) {
                auto* methodDecl = dynamic_cast<FunctionDecl*>(classStmt.get());
                if (!methodDecl || methodDecl->name == "__init__") continue;
                if (!methodDecl->typeParams.empty()) continue;

                if (isReservedDunder(methodDecl->name)) {
                    classDunderMethodsBySym[csym].insert(methodDecl->name);
                }

                std::string methodName = clsSym + "_" + methodDecl->name;
                if (methodDecl->methodOverloadCount > 1 &&
                    methodDecl->methodOverloadIndex >= 0)
                    methodName += "__ov" + std::to_string(methodDecl->methodOverloadIndex);
                if (module->getFunction(methodName)) continue;

                std::vector<llvm::Type*> methodParamTypes;
                if (!methodDecl->isStatic) {
                    methodParamTypes.push_back(i8PtrType);
                } else {
                    staticMethods.insert(methodName);
                }
                size_t mParamStart;
                if (methodDecl->isClassMethod) {
                    mParamStart = 1;
                } else if (methodDecl->isStatic || methodDecl->hasImplicitSelf) {
                    mParamStart = 0;
                } else {
                    mParamStart = 1;
                }
                VarArgInfo vaInfo;
                bool seenVarArg = false;
                for (size_t i = mParamStart; i < methodDecl->params.size(); ++i) {
                    const auto& p = methodDecl->params[i];
                    if (p.isVarArg) {
                        seenVarArg = true;
                        if (p.name.empty()) continue;
                        vaInfo.hasVarArg = true;
                        vaInfo.varArgName = p.name;
                        if (p.type) {
                            Type::Kind tk =
                                elemVarKindToTypeKind(typeExprToKind(p.type.get()));
                            vaInfo.varArgElemTag = typeKindToElemTag(tk);
                            vaInfo.varArgElemIsAny = (tk == Type::Kind::Any);
                        }
                        methodParamTypes.push_back(i8PtrType);
                        continue;
                    }
                    if (p.isKwArg) {
                        seenVarArg = true;
                        vaInfo.hasKwArg = true;
                        vaInfo.kwArgName = p.name;
                        methodParamTypes.push_back(i8PtrType);
                        continue;
                    }
                    if (!seenVarArg) vaInfo.numRegularParams++;
                    methodParamTypes.push_back(typeExprToLLVM(p.type.get()));
                }

                bool methodIsGenerator =
                    containsYield(methodDecl->body) && !methodDecl->isClassMethod;
                bool methodIsAsync =
                    methodDecl->isAsync && !methodDecl->isClassMethod;
                llvm::Type* retType =
                    (methodIsGenerator || methodIsAsync) ? i8PtrType
                    : (methodDecl->returnType
                        ? typeExprToLLVM(methodDecl->returnType.get())
                        : unannotatedReturnType(methodDecl->body));
                auto* methodFuncType = llvm::FunctionType::get(retType, methodParamTypes, false);
                llvm::Function::Create(methodFuncType, llvm::Function::InternalLinkage,
                                       methodName, module.get());
                if (vaInfo.hasVarArg || vaInfo.hasKwArg)
                    funcVarArgInfo[methodName] = vaInfo;
                if (methodIsGenerator) {
                    generatorFunctions.insert(methodName);
                    generatorYieldKinds[methodName] = inferYieldKind(methodDecl->body);
                }

                if (options.gcMode == GCMode::RC) {
                    std::vector<VarKind> mkinds;
                    std::vector<bool> mowns;
                    if (!methodDecl->isStatic) {
                        mkinds.push_back(VarKind::ClassInstance);
                        mowns.push_back(false);
                    }
                    for (size_t i = mParamStart; i < methodDecl->params.size(); ++i) {
                        const auto& p = methodDecl->params[i];
                        if (p.isVarArg && p.name.empty()) continue;
                        if (p.isVarArg) {
                            mkinds.push_back(VarKind::List); mowns.push_back(false);
                            continue;
                        }
                        if (p.isKwArg) {
                            mkinds.push_back(VarKind::Dict); mowns.push_back(false);
                            continue;
                        }
                        mkinds.push_back(typeExprToKind(p.type.get()));
                        mowns.push_back(p.isOwn);
                    }
                    funcParamKinds[methodName] = std::move(mkinds);
                    funcParamOwns[methodName] = std::move(mowns);
                }

                {
                    std::vector<Expr*> defaults;
                    if (!methodDecl->isStatic) {
                        defaults.push_back(nullptr);
                    }
                    for (size_t i = mParamStart; i < methodDecl->params.size(); ++i) {
                        const auto& p = methodDecl->params[i];
                        if (p.isVarArg && p.name.empty()) continue;
                        if (p.isVarArg || p.isKwArg) {
                            defaults.push_back(nullptr);
                            continue;
                        }
                        defaults.push_back(p.defaultValue.get());
                    }
                    funcParamDefaults[methodName] = std::move(defaults);
                    funcDefiningModule[methodName] = currentModuleName;
                }
                {
                    std::vector<std::string> names;
                    if (!methodDecl->isStatic) {
                        names.push_back("self");
                    }
                    for (size_t i = mParamStart; i < methodDecl->params.size(); ++i) {
                        const auto& p = methodDecl->params[i];
                        if (p.isVarArg && p.name.empty()) continue;
                        names.push_back(p.name);
                    }
                    funcParamNames[methodName] = std::move(names);
                }

                if (auto* retNamed = dynamic_cast<NamedTypeExpr*>(methodDecl->returnType.get())) {
                    if (classNames.count(retNamed->name))
                        methodReturnClassNames[methodName] = retNamed->name;
                }
                if (methodDecl->returnType)
                    methodReturnKinds[methodName] =
                        typeExprToTypeKind(methodDecl->returnType.get());

                if (methodDecl->isProperty) {
                    classPropertiesBySym[csym].insert(methodDecl->name);
                }
                if (!methodDecl->propertySetterFor.empty()) {
                    classPropertySettersBySym[csym][methodDecl->propertySetterFor] =
                        methodDecl->name;
                }
            }

            {
                std::vector<std::string> vtableOrder;
                std::vector<std::string> ownMethods;
                auto parentIt = classParentNamesBySym.find(csym);
                if (parentIt != classParentNamesBySym.end()) {
                    auto poIt = classVtableMethodOrderBySym.find(parentIt->second);
                    if (poIt != classVtableMethodOrderBySym.end())
                        vtableOrder = poIt->second;
                }
                for (auto& classStmt : classDecl->body) {
                    auto* md = dynamic_cast<FunctionDecl*>(classStmt.get());
                    if (!md) continue;
                    uint8_t kind = 0;
                    if (md->isClassMethod) kind = 2;
                    else if (md->isStatic) kind = 1;
                    classMethodKindsBySym[csym][md->name] = kind;
                    if (md->name == "__init__") continue;
                    ownMethods.push_back(md->name);
                    bool found = false;
                    for (auto& existing : vtableOrder) {
                        if (existing == md->name) { found = true; break; }
                    }
                    if (!found) vtableOrder.push_back(md->name);
                }
                classVtableMethodOrderBySym[csym] = vtableOrder;
                classOwnMethodsBySym[csym] = ownMethods;
                for (size_t i = 0; i < vtableOrder.size(); ++i) {
                    classMethodVtableIndicesBySym[csym][vtableOrder[i]] = (unsigned)i;
                }
            }
        }
    }
}

void CodeGen::Impl::collectContracts(dragon::Module& mod) {
    for (auto& stmt : mod.body) {
        auto* cd = dynamic_cast<ContractDecl*>(stmt.get());
        if (!cd) continue;
        if (!contractDeclSeen.insert(cd).second) continue;
        contractDeclsInOrder.push_back(cd);
        contractTypeNames.insert(cd->name);
    }
}

void CodeGen::Impl::assignContractSlots() {
    if (contractSlotsAssigned) return;
    contractSlotsAssigned = true;
    if (contractDeclsInOrder.empty()) return;

    std::unordered_map<std::string, std::set<const ContractDecl*>> ownAtoms;
    auto gather = [&](dragon::Module* m) {
        if (!m) return;
        for (auto& stmt : m->body) {
            auto* cd = dynamic_cast<ClassDecl*>(stmt.get());
            if (!cd || cd->conformedContracts.empty()) continue;
            const std::string csym = mangleClass(m->moduleName, cd->name);
            ownAtoms[csym].insert(cd->conformedContracts.begin(),
                                  cd->conformedContracts.end());
        }
    };
    for (auto* dep : depModulePtrs) gather(dep);
    gather(entryModulePtr);

    std::unordered_map<std::string, std::set<const ContractDecl*>> effAtoms;
    for (auto& [csym, order] : classVtableMethodOrderBySym) {
        std::set<const ContractDecl*> atoms;
        std::string cur = csym;
        std::set<std::string> guard;
        while (!cur.empty() && guard.insert(cur).second) {
            auto oIt = ownAtoms.find(cur);
            if (oIt != ownAtoms.end())
                atoms.insert(oIt->second.begin(), oIt->second.end());
            auto pIt = classParentNamesBySym.find(cur);
            cur = (pIt != classParentNamesBySym.end()) ? pIt->second : "";
        }
        if (!atoms.empty()) effAtoms[csym] = std::move(atoms);
    }
    if (effAtoms.empty()) return;

    unsigned base = 0;
    for (auto& [csym, order] : classVtableMethodOrderBySym)
        base = std::max(base, (unsigned)order.size());
    unsigned next = base;
    for (auto* cd : contractDeclsInOrder)
        for (auto& m : cd->methods)
            contractMethodSlots[{cd, m->name}] = next++;

    for (auto& [csym, atoms] : effAtoms) {
        auto voIt = classVtableMethodOrderBySym.find(csym);
        if (voIt == classVtableMethodOrderBySym.end()) continue;
        auto& order = voIt->second;
        for (auto* atom : atoms) {
            for (auto& m : atom->methods) {
                unsigned slot = contractMethodSlots[{atom, m->name}];
                if (order.size() <= slot) order.resize(slot + 1, "");
                order[slot] = m->name;
            }
        }
    }
}

}
