/// Dragon CodeGen - Impl initialization, runtime declarations, and forward declarations.
/// Extracted from CodeGenImpl.h to reduce header size.
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

    // Must be set now, not before compileToObject: struct offsets baked into
    // IR during building would otherwise mismatch the final target layout.
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
    // intc = C's int: i16 on 16-bit targets, i32 on 32/64-bit (covers x86_64, ARM64, etc.)
    llvm::Triple triple(options.targetTriple);
    intcType = triple.isArch16Bit()
        ? llvm::Type::getInt16Ty(*context)
        : llvm::Type::getInt32Ty(*context);
    f64Type = llvm::Type::getDoubleTy(*context);
    i1Type = llvm::Type::getInt1Ty(*context);
    i8PtrType = llvm::PointerType::getUnqual(*context);
    voidType = llvm::Type::getVoidTy(*context);

    // D030 Phase 4: %dragon.box = { i64 tag, i64 payload }
    boxType = llvm::StructType::create(
        *context, {i64Type, i64Type}, "dragon.box");

    // TBAA tree for alias analysis: tells LLVM that list struct fields (data
    // ptr, size) don't alias element array data, enabling LICM in loops.
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
    // void dragon_print_int(i64)
    getOrDeclareRuntime("dragon_print_int",
        llvm::FunctionType::get(voidType, {i64Type}, false));
    // void dragon_print_float(double)
    getOrDeclareRuntime("dragon_print_float",
        llvm::FunctionType::get(voidType, {f64Type}, false));
    // void dragon_print_str(i8*)
    getOrDeclareRuntime("dragon_print_str",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_print_bool(i64)
    getOrDeclareRuntime("dragon_print_bool",
        llvm::FunctionType::get(voidType, {i64Type}, false));
    // void dragon_print_none()
    getOrDeclareRuntime("dragon_print_none",
        llvm::FunctionType::get(voidType, {}, false));
    // void dragon_print_newline()
    getOrDeclareRuntime("dragon_print_newline",
        llvm::FunctionType::get(voidType, {}, false));
    // Multi-arg print() support: `_raw` printers (no trailing newline) +
    // a single-space separator. Same signatures as their public counterparts.
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
    // C5: nested-container printers (route through the recursive repr builders).
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
    // const char* dragon_input(const char*)
    getOrDeclareRuntime("dragon_input",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // const char* dragon_str_concat(const char*, const char*)
    getOrDeclareRuntime("dragon_str_concat",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    // const char* dragon_str_append_inplace(const char*, const char*)
    getOrDeclareRuntime("dragon_str_append_inplace",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    // i64 dragon_str_len(const char*)
    getOrDeclareRuntime("dragon_str_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // i64 dragon_str_eq(const char*, const char*)
    getOrDeclareRuntime("dragon_str_eq",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // i64 dragon_str_cmp(const char*, const char*) - returns <0, 0, >0
    getOrDeclareRuntime("dragon_str_cmp",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // i64 dragon_str_contains(const char*, const char*)
    getOrDeclareRuntime("dragon_str_contains",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // void dragon_assert(i64, const char*)
    getOrDeclareRuntime("dragon_assert",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    // void dragon_assert_no_msg(i64)
    getOrDeclareRuntime("dragon_assert_no_msg",
        llvm::FunctionType::get(voidType, {i64Type}, false));
    // i64 dragon_pow_int(i64, i64)
    getOrDeclareRuntime("dragon_pow_int",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    // i64 dragon_pow_int_checked(i64, i64) - raises on overflow
    getOrDeclareRuntime("dragon_pow_int_checked",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    // i64 dragon_floordiv_int(i64, i64)
    getOrDeclareRuntime("dragon_floordiv_int",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    // i64 dragon_mod_int(i64, i64)
    getOrDeclareRuntime("dragon_mod_int",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    // i64 dragon_abs_int(i64)
    getOrDeclareRuntime("dragon_abs_int",
        llvm::FunctionType::get(i64Type, {i64Type}, false));
    // double dragon_abs_float(double)
    getOrDeclareRuntime("dragon_abs_float",
        llvm::FunctionType::get(f64Type, {f64Type}, false));
    // const char* dragon_int_to_str(i64)
    getOrDeclareRuntime("dragon_int_to_str",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    // const char* dragon_float_to_str(double)
    getOrDeclareRuntime("dragon_float_to_str",
        llvm::FunctionType::get(i8PtrType, {f64Type}, false));
    // const char* dragon_float_format(double, ptr spec)
    getOrDeclareRuntime("dragon_float_format",
        llvm::FunctionType::get(i8PtrType, {f64Type, i8PtrType}, false));
    // const char* dragon_int_format(i64, ptr spec)
    getOrDeclareRuntime("dragon_int_format",
        llvm::FunctionType::get(i8PtrType, {i64Type, i8PtrType}, false));

    // --- List operations ---
    // ptr dragon_list_new(i64 capacity)
    getOrDeclareRuntime("dragon_list_new",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    // ptr dragon_list_new_tagged(i64 capacity, i64 elem_tag)
    getOrDeclareRuntime("dragon_list_new_tagged",
        llvm::FunctionType::get(i8PtrType, {i64Type, i64Type}, false));
    // ptr dragon_list_repeat(ptr src, i64 count)
    getOrDeclareRuntime("dragon_list_repeat",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    // void dragon_list_append(ptr list, i64 value)
    getOrDeclareRuntime("dragon_list_append",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // i64 dragon_list_get(ptr list, i64 index)
    getOrDeclareRuntime("dragon_list_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // void dragon_list_set(ptr list, i64 index, i64 value)
    getOrDeclareRuntime("dragon_list_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));

    // --- D030 Phase 3: monomorphized list ops (typed return / accept) ---
    // list[float] - native f64
    // ptr dragon_list_new_f64(i64 capacity)
    getOrDeclareRuntime("dragon_list_new_f64",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    // double dragon_list_get_f64(ptr list, i64 index)
    getOrDeclareRuntime("dragon_list_get_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType, i64Type}, false));
    // void dragon_list_set_f64(ptr list, i64 index, double value)
    getOrDeclareRuntime("dragon_list_set_f64",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, f64Type}, false));
    // void dragon_list_append_f64(ptr list, double value)
    getOrDeclareRuntime("dragon_list_append_f64",
        llvm::FunctionType::get(voidType, {i8PtrType, f64Type}, false));
    // list[<heap>] - native ptr (refcount-aware ops)
    // ptr dragon_list_new_ptr(i64 capacity, i64 elem_tag)
    getOrDeclareRuntime("dragon_list_new_ptr",
        llvm::FunctionType::get(i8PtrType, {i64Type, i64Type}, false));
    // ptr dragon_list_get_ptr(ptr list, i64 index)
    getOrDeclareRuntime("dragon_list_get_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    // void dragon_list_set_ptr(ptr list, i64 index, ptr value)
    getOrDeclareRuntime("dragon_list_set_ptr",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i8PtrType}, false));
    // void dragon_list_append_ptr(ptr list, ptr value)
    getOrDeclareRuntime("dragon_list_append_ptr",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    // ptr dragon_str_join_ptr(ptr sep, ptr list) - used by template block
    // interpolation (Phase 4.B) and the `| join` filter (Phase 4.C).
    getOrDeclareRuntime("dragon_str_join_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    // i64 dragon_list_len(ptr list)
    getOrDeclareRuntime("dragon_list_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // void dragon_print_list_int(ptr list)
    getOrDeclareRuntime("dragon_print_list_int",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_print_list_str(ptr list)
    getOrDeclareRuntime("dragon_print_list_str",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_print_list_float(ptr list)
    getOrDeclareRuntime("dragon_print_list_float",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_print_list_bool(ptr list)
    getOrDeclareRuntime("dragon_print_list_bool",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_list_insert(ptr list, i64 index, i64 value)
    getOrDeclareRuntime("dragon_list_insert",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    // void dragon_list_remove(ptr list, i64 value)
    getOrDeclareRuntime("dragon_list_remove",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // i64 dragon_list_pop(ptr list, i64 index)
    getOrDeclareRuntime("dragon_list_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // double dragon_list_pop_f64(ptr list, i64 index) - typed pop for
    // list[float] so the value flows at native f64 (no SIToFP bit corruption).
    getOrDeclareRuntime("dragon_list_pop_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType, i64Type}, false));
    // void dragon_list_delitem(ptr list, i64 index) - del lst[i]
    getOrDeclareRuntime("dragon_list_delitem",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // void dragon_list_box_delitem(ptr list, i64 index) - del lst[i] on list[Any]
    getOrDeclareRuntime("dragon_list_box_delitem",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // void dragon_list_clear(ptr list)
    getOrDeclareRuntime("dragon_list_clear",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_list_extend(ptr list, ptr other)
    getOrDeclareRuntime("dragon_list_extend",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    // i64 dragon_list_index(ptr list, i64 value)
    getOrDeclareRuntime("dragon_list_index",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // i64 dragon_list_count(ptr list, i64 value)
    getOrDeclareRuntime("dragon_list_count",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // i64 dragon_list_contains(ptr list, i64 value)
    getOrDeclareRuntime("dragon_list_contains",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // ptr dragon_{list,dict,set,tuple}_to_str(ptr) - str()/f-string of a container
    getOrDeclareRuntime("dragon_list_to_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ptr dragon_list_box_to_str(ptr) - str()/f-string of a list[Any] (16B/elem)
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
    // void dragon_list_sort(ptr list)
    getOrDeclareRuntime("dragon_list_sort",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_list_reverse(ptr list)
    getOrDeclareRuntime("dragon_list_reverse",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // ptr dragon_list_copy(ptr list)
    getOrDeclareRuntime("dragon_list_copy",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));

    // --- String indexing ---
    // ptr dragon_str_index(ptr str, i64 index)
    getOrDeclareRuntime("dragon_str_index",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));

    // --- Slice operations ---
    // ptr dragon_str_slice(ptr str, i64 start, i64 stop, i64 step)
    getOrDeclareRuntime("dragon_str_slice",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    // ptr dragon_list_slice(ptr list, i64 start, i64 stop, i64 step)
    getOrDeclareRuntime("dragon_list_slice",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i64Type, i64Type}, false));

    // --- Bool to string (for f-strings) ---
    getOrDeclareRuntime("dragon_bool_to_str",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));

    // --- String repeat ---
    getOrDeclareRuntime("dragon_str_repeat",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));

    // --- Dict operations ---
    // ptr dragon_dict_new(i64 capacity)
    getOrDeclareRuntime("dragon_dict_new",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    // void dragon_dict_set(ptr dict, ptr key, i64 value)
    getOrDeclareRuntime("dragon_dict_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType, i64Type}, false));
    // void dragon_dict_set_tagged(ptr dict, ptr key, i64 value, i64 tag)
    getOrDeclareRuntime("dragon_dict_set_tagged",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType, i64Type, i64Type}, false));
    // i64 dragon_dict_str_iaug_i64(ptr dict, ptr key, i64 operand, i64 op)
    getOrDeclareRuntime("dragon_dict_str_iaug_i64",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type, i64Type}, false));
    // i64 dragon_dict_get(ptr dict, ptr key)
    getOrDeclareRuntime("dragon_dict_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // i64 dragon_dict_get_tag(ptr dict, ptr key)
    getOrDeclareRuntime("dragon_dict_get_tag",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // i64 dragon_dict_get_checked(ptr dict, ptr key, i64 expected_tag)
    getOrDeclareRuntime("dragon_dict_get_checked",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    // dragon_dict_get_box returns {tag, payload} by value (2 registers). Borrow
    // contract: caller increfs the payload by tag before storing it long-lived.
    getOrDeclareRuntime("dragon_dict_get_box",
        llvm::FunctionType::get(boxType, {i8PtrType, i8PtrType}, false));
    // D039 Phase 3: void dragon_print_box(%dragon.box)
    // Tag-dispatched print for Any/Union/Optional values.
    getOrDeclareRuntime("dragon_print_box",
        llvm::FunctionType::get(voidType, {boxType}, false));

    // list[Any] runtime ops: per-element {tag, payload} storage, 16 bytes/elem
    // like Go's []interface{} - single cache miss per read.
    // ptr dragon_list_box_new(i64 capacity)
    getOrDeclareRuntime("dragon_list_box_new",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    // %dragon.box dragon_list_box_get(ptr list, i64 index)
    getOrDeclareRuntime("dragon_list_box_get",
        llvm::FunctionType::get(boxType, {i8PtrType, i64Type}, false));
    // dragon_list_view_check raises TypeError when a box-tagged list's real
    // representation (DragonList vs DragonListBox, elem_tag) mismatches the typed view.
    getOrDeclareRuntime("dragon_list_view_check",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // dragon_box_len: len() of an Any value, tag+header dispatched across
    // str/list/dict/bytes; raises TypeError for unsized values.
    getOrDeclareRuntime("dragon_box_len",
        llvm::FunctionType::get(i64Type, {boxType}, false));
    // void dragon_list_box_set(ptr list, i64 index, i64 tag, i64 payload)
    getOrDeclareRuntime("dragon_list_box_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    // void dragon_list_box_append(ptr list, i64 tag, i64 payload)
    getOrDeclareRuntime("dragon_list_box_append",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    // %dragon.box dragon_list_box_pop(ptr list, i64 index)
    getOrDeclareRuntime("dragon_list_box_pop",
        llvm::FunctionType::get(boxType, {i8PtrType, i64Type}, false));
    // void dragon_list_box_remove(ptr list, i64 tag, i64 payload)
    getOrDeclareRuntime("dragon_list_box_remove",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    // void dragon_list_box_insert(ptr list, i64 index, i64 tag, i64 payload)
    getOrDeclareRuntime("dragon_list_box_insert",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    // void dragon_list_box_destroy(ptr list)
    getOrDeclareRuntime("dragon_list_box_destroy",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_print_list_box(ptr list)
    getOrDeclareRuntime("dragon_print_list_box",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    // D039 Phase 10: i64 dragon_box_eq(box a, box b) - tag-then-payload compare.
    getOrDeclareRuntime("dragon_box_eq",
        llvm::FunctionType::get(i64Type, {boxType, boxType}, false));
    // D039 Phase 11: box dragon_box_binop(box a, box b, i64 op) - Any arithmetic
    // (+ - * / // % **) with runtime tag dispatch; returns a result box.
    getOrDeclareRuntime("dragon_box_binop",
        llvm::FunctionType::get(boxType, {boxType, boxType, i64Type}, false));
    // D039 Phase 11b: i64 dragon_box_cmp(box a, box b, i64 op) - Any ordering
    // (< <= > >=); three-way result (<0/0/>0). op is for the TypeError message.
    getOrDeclareRuntime("dragon_box_cmp",
        llvm::FunctionType::get(i64Type, {boxType, boxType, i64Type}, false));
    // ptr dragon_box_to_str(box) - str(anyValue) / f-string interpolation
    // of an Any-typed value. Returns an owned refcounted heap DragonString.
    getOrDeclareRuntime("dragon_box_to_str",
        llvm::FunctionType::get(i8PtrType, {boxType}, false));
    // dragon_box_subscript = `anyVal[i]`: tag-dispatched read across Any-boxed
    // list/dict/str/bytes; returns an OWNED box (borrowed elements incref'd).
    getOrDeclareRuntime("dragon_box_subscript",
        llvm::FunctionType::get(boxType, {boxType, boxType}, false));
    // dragon_box_decref releases an owned box temp's payload by tag (no-op if
    // non-refcounted); frees box_binop/box_subscript results after transient use.
    getOrDeclareRuntime("dragon_box_decref",
        llvm::FunctionType::get(voidType, {boxType}, false));

    // unittest deep-equality support: dragon_box_eq recurses through these;
    // codegen calls them directly for list==list/dict==dict when neither side is boxed.
    getOrDeclareRuntime("dragon_list_eq",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // i64 dragon_list_cmp(ptr a, ptr b) - lexicographic three-way (<0/0/>0) for
    // native `list < list` ordering (Python semantics; was a pointer compare).
    getOrDeclareRuntime("dragon_list_cmp",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_eq",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_dict_int_eq",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));

    // D039 Phase 9: ptr dragon_dict_values_box(ptr d) - materialize a
    // DragonListBox from a dict[str, Any] for `for v in cfg.values()`.
    getOrDeclareRuntime("dragon_dict_values_box",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));

    // --- D030 Phase 3.E: typed dict ops for str-keyed monomorphic dicts ---
    // double dragon_dict_get_str_f64(ptr dict, ptr key)
    getOrDeclareRuntime("dragon_dict_get_str_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType, i8PtrType}, false));
    // void dragon_dict_set_str_f64(ptr dict, ptr key, double value)
    getOrDeclareRuntime("dragon_dict_set_str_f64",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType, f64Type}, false));
    // ptr dragon_dict_get_str_ptr(ptr dict, ptr key, i64 expected_tag)
    getOrDeclareRuntime("dragon_dict_get_str_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type}, false));
    // void dragon_dict_set_str_ptr(ptr dict, ptr key, ptr value, i64 tag)
    getOrDeclareRuntime("dragon_dict_set_str_ptr",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType, i8PtrType, i64Type}, false));

    // --- D030 Phase 3.G: typed dict ops for int-keyed monomorphic dicts ---
    // void dragon_dict_int_set(ptr dict, i64 key, i64 value)
    getOrDeclareRuntime("dragon_dict_int_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    // void dragon_dict_int_set_tagged(ptr dict, i64 key, i64 value, i64 tag)
    getOrDeclareRuntime("dragon_dict_int_set_tagged",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    // void dragon_dict_int_set_f64(ptr dict, i64 key, double value)
    getOrDeclareRuntime("dragon_dict_int_set_f64",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, f64Type}, false));
    // void dragon_dict_int_set_str(ptr dict, i64 key, ptr value)
    getOrDeclareRuntime("dragon_dict_int_set_str",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i8PtrType}, false));
    // void dragon_dict_int_set_ptr(ptr dict, i64 key, ptr value, i64 tag)
    getOrDeclareRuntime("dragon_dict_int_set_ptr",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i8PtrType, i64Type}, false));
    // i64 dragon_dict_int_get(ptr dict, i64 key)
    getOrDeclareRuntime("dragon_dict_int_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // i64 dragon_dict_int_get_tag(ptr dict, i64 key)
    getOrDeclareRuntime("dragon_dict_int_get_tag",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // i64 dragon_dict_int_get_checked(ptr dict, i64 key, i64 expected_tag)
    getOrDeclareRuntime("dragon_dict_int_get_checked",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, i64Type}, false));
    // double dragon_dict_int_get_f64(ptr dict, i64 key)
    getOrDeclareRuntime("dragon_dict_int_get_f64",
        llvm::FunctionType::get(f64Type, {i8PtrType, i64Type}, false));
    // ptr dragon_dict_int_get_str(ptr dict, i64 key)
    getOrDeclareRuntime("dragon_dict_int_get_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    // ptr dragon_dict_int_get_ptr(ptr dict, i64 key, i64 expected_tag)
    getOrDeclareRuntime("dragon_dict_int_get_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i64Type}, false));
    // i64 dragon_dict_int_get_default(ptr dict, i64 key, i64 default)
    getOrDeclareRuntime("dragon_dict_int_get_default",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, i64Type}, false));
    // i64 dragon_dict_int_has_key(ptr dict, i64 key)
    getOrDeclareRuntime("dragon_dict_int_has_key",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // i64 dragon_dict_int_pop(ptr dict, i64 key)
    getOrDeclareRuntime("dragon_dict_int_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // void dragon_dict_int_del(ptr dict, i64 key) - `del d[k]`, int-keyed
    getOrDeclareRuntime("dragon_dict_int_del",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // i64 dragon_dict_int_pop_default(ptr dict, i64 key, i64 default)
    getOrDeclareRuntime("dragon_dict_int_pop_default",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, i64Type}, false));
    // i64 dragon_dict_int_setdefault(ptr dict, i64 key, i64 default)
    getOrDeclareRuntime("dragon_dict_int_setdefault",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, i64Type}, false));
    // ptr dragon_dict_int_keys(ptr dict)
    getOrDeclareRuntime("dragon_dict_int_keys",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // void dragon_print_dict_int(ptr dict)
    getOrDeclareRuntime("dragon_print_dict_int",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_print_tagged(i64 value, i64 tag)
    getOrDeclareRuntime("dragon_print_tagged",
        llvm::FunctionType::get(voidType, {i64Type, i64Type}, false));
    // i64 dragon_dict_len(ptr dict)
    getOrDeclareRuntime("dragon_dict_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // i64 dragon_dict_has_key(ptr dict, ptr key)
    getOrDeclareRuntime("dragon_dict_has_key",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // void dragon_dict_reject_unknown_keys(ptr dict, ptr allowed[], i64 n, ptr fname)
    getOrDeclareRuntime("dragon_dict_reject_unknown_keys",
        llvm::FunctionType::get(voidType,
            {i8PtrType, i8PtrType, i64Type, i8PtrType}, false));
    // i64 dragon_dict_get_default(ptr dict, ptr key, i64 default)
    getOrDeclareRuntime("dragon_dict_get_default",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    // ptr dragon_dict_get_str_default(ptr dict, ptr key, ptr default) -> OWNED str
    getOrDeclareRuntime("dragon_dict_get_str_default",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i8PtrType}, false));
    // ptr dragon_dict_get_ptr(ptr dict, ptr key) -> OWNED heap value (incref'd)
    getOrDeclareRuntime("dragon_dict_get_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    // ptr dragon_dict_get_ptr_default(ptr dict, ptr key, ptr default) -> OWNED heap value
    getOrDeclareRuntime("dragon_dict_get_ptr_default",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i8PtrType}, false));
    // ptr dragon_dict_int_get_owned(ptr dict, i64 key) -> OWNED heap value
    getOrDeclareRuntime("dragon_dict_int_get_owned",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    // ptr dragon_dict_int_get_owned_default(ptr dict, i64 key, ptr default) -> OWNED
    getOrDeclareRuntime("dragon_dict_int_get_owned_default",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i8PtrType}, false));
    // ptr dragon_dict_setdefault_ptr(ptr dict, ptr key, ptr default, i64 tag) -> OWNED heap value
    getOrDeclareRuntime("dragon_dict_setdefault_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i8PtrType, i64Type}, false));
    // ptr dragon_dict_int_setdefault_owned(ptr dict, i64 key, ptr default, i64 tag) -> OWNED
    getOrDeclareRuntime("dragon_dict_int_setdefault_owned",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i8PtrType, i64Type}, false));
    // ptr dragon_dict_keys(ptr dict) -> DragonList*
    getOrDeclareRuntime("dragon_dict_keys",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // void dragon_print_dict(ptr dict)
    getOrDeclareRuntime("dragon_print_dict",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // ptr dragon_dict_values(ptr dict) -> DragonList*
    getOrDeclareRuntime("dragon_dict_values",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ptr dragon_dict_items(ptr dict) -> DragonList*
    getOrDeclareRuntime("dragon_dict_items",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // i64 dragon_dict_pop(ptr dict, ptr key)
    getOrDeclareRuntime("dragon_dict_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // void dragon_dict_del(ptr dict, ptr key) - `del d[k]`, str-keyed
    getOrDeclareRuntime("dragon_dict_del",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    // i64 dragon_dict_pop_default(ptr dict, ptr key, i64 default)
    getOrDeclareRuntime("dragon_dict_pop_default",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    // i64 dragon_dict_popitem(ptr dict) -> DragonTuple* (cast to i64)
    getOrDeclareRuntime("dragon_dict_popitem",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // ptr dragon_dict_fromkeys(ptr keys_list, i64 value, i64 tag) -> DragonDict*
    getOrDeclareRuntime("dragon_dict_fromkeys",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type, i64Type}, false));
    // void dragon_dict_clear(ptr dict)
    getOrDeclareRuntime("dragon_dict_clear",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_dict_update(ptr dict, ptr other)
    getOrDeclareRuntime("dragon_dict_update",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    // i64 dragon_dict_setdefault(ptr dict, ptr key, i64 default)
    getOrDeclareRuntime("dragon_dict_setdefault",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType, i64Type}, false));
    // ptr dragon_dict_copy(ptr dict) -> DragonDict*
    getOrDeclareRuntime("dragon_dict_copy",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ptr dragon_dict_copy_excluding(ptr dict, ptr names[], i64 n) -> DragonDict*
    getOrDeclareRuntime("dragon_dict_copy_excluding",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type}, false));

    // --- Tuple runtime ---
    // ptr dragon_tuple_new(i64 count)
    getOrDeclareRuntime("dragon_tuple_new",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    // i64 dragon_tuple_get(ptr tuple, i64 index)
    getOrDeclareRuntime("dragon_tuple_get",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // %dragon.box dragon_tuple_box_get(ptr tuple, i64 index) - Any/Union
    // element read; BORROW contract like dragon_list_box_get.
    getOrDeclareRuntime("dragon_tuple_box_get",
        llvm::FunctionType::get(boxType, {i8PtrType, i64Type}, false));
    // void dragon_tuple_set(ptr tuple, i64 index, i64 value)
    getOrDeclareRuntime("dragon_tuple_set",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    // void dragon_tuple_set_tagged(ptr tuple, i64 index, i64 value, i64 tag)
    getOrDeclareRuntime("dragon_tuple_set_tagged",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type, i64Type}, false));
    // i64 dragon_tuple_len(ptr tuple)
    getOrDeclareRuntime("dragon_tuple_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // void dragon_print_tuple(ptr tuple)
    getOrDeclareRuntime("dragon_print_tuple",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    // --- Set runtime ---
    // ptr dragon_set_new()
    getOrDeclareRuntime("dragon_set_new",
        llvm::FunctionType::get(i8PtrType, {}, false));
    // ptr dragon_set_new_tagged(i64 elem_tag)
    getOrDeclareRuntime("dragon_set_new_tagged",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    // ptr dragon_set_from_list(ptr list)
    getOrDeclareRuntime("dragon_set_from_list",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // void dragon_set_adopt_tag(ptr set, i64 tag)
    getOrDeclareRuntime("dragon_set_adopt_tag",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // void dragon_set_add(ptr set, i64 value)
    getOrDeclareRuntime("dragon_set_add",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // i64 dragon_set_contains(ptr set, i64 value)
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
    // void dragon_set_remove(ptr set, i64 value)
    getOrDeclareRuntime("dragon_set_remove",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // void dragon_set_discard(ptr set, i64 value)
    getOrDeclareRuntime("dragon_set_discard",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // i64 dragon_set_len(ptr set)
    getOrDeclareRuntime("dragon_set_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // void dragon_set_clear(ptr set)
    getOrDeclareRuntime("dragon_set_clear",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // ptr dragon_set_copy(ptr set)
    getOrDeclareRuntime("dragon_set_copy",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ptr dragon_set_union(ptr a, ptr b)
    getOrDeclareRuntime("dragon_set_union",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    // ptr dragon_set_intersection(ptr a, ptr b)
    getOrDeclareRuntime("dragon_set_intersection",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    // ptr dragon_set_difference(ptr a, ptr b)
    getOrDeclareRuntime("dragon_set_difference",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    // ptr dragon_set_symmetric_difference(ptr a, ptr b)
    getOrDeclareRuntime("dragon_set_symmetric_difference",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    // i64 dragon_set_issubset(ptr a, ptr b)
    getOrDeclareRuntime("dragon_set_issubset",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // i64 dragon_set_issuperset(ptr a, ptr b)
    getOrDeclareRuntime("dragon_set_issuperset",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // i64 dragon_set_isdisjoint(ptr a, ptr b)
    getOrDeclareRuntime("dragon_set_isdisjoint",
        llvm::FunctionType::get(i64Type, {i8PtrType, i8PtrType}, false));
    // i64 dragon_set_pop(ptr set)
    getOrDeclareRuntime("dragon_set_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // void dragon_set_update(ptr a, ptr b)
    getOrDeclareRuntime("dragon_set_update",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    // void dragon_print_set(ptr set)
    getOrDeclareRuntime("dragon_print_set",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    // --- Deque (collections.deque) ---
    // ptr dragon_deque_new(i64 maxlen, i64 elem_tag) - maxlen -1 = unbounded
    getOrDeclareRuntime("dragon_deque_new",
        llvm::FunctionType::get(i8PtrType, {i64Type, i64Type}, false));
    // void dragon_deque_append(ptr deque, i64 value, i64 tag)
    getOrDeclareRuntime("dragon_deque_append",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    // void dragon_deque_appendleft(ptr deque, i64 value, i64 tag)
    getOrDeclareRuntime("dragon_deque_appendleft",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    // i64 dragon_deque_popleft(ptr deque)
    getOrDeclareRuntime("dragon_deque_popleft",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // i64 dragon_deque_pop(ptr deque)
    getOrDeclareRuntime("dragon_deque_pop",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // dragon_deque_popleft_ptr/pop_ptr: ptr-return pop variants recognized as an
    // OWNED ptr (drained/adopted like dragon_dict_get_ptr, #19).
    getOrDeclareRuntime("dragon_deque_popleft_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_deque_pop_ptr",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // i64 dragon_deque_len(ptr deque)
    getOrDeclareRuntime("dragon_deque_len",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // i64 dragon_deque_contains(ptr deque, i64 value)
    getOrDeclareRuntime("dragon_deque_contains",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));
    // ptr dragon_deque_from_list(ptr list, i64 maxlen)
    getOrDeclareRuntime("dragon_deque_from_list",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    // ptr dragon_deque_to_str(ptr deque)
    getOrDeclareRuntime("dragon_deque_to_str",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // void dragon_print_deque_raw(ptr deque)
    getOrDeclareRuntime("dragon_print_deque_raw",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_deque_destroy(ptr deque)
    getOrDeclareRuntime("dragon_deque_destroy",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    // --- Exception handling ---
    // ptr dragon_exc_push_frame()
    getOrDeclareRuntime("dragon_exc_push_frame",
        llvm::FunctionType::get(i8PtrType, {}, false));
    // void dragon_exc_pop_frame()
    getOrDeclareRuntime("dragon_exc_pop_frame",
        llvm::FunctionType::get(voidType, {}, false));
    // i64 dragon_exc_get_type()
    getOrDeclareRuntime("dragon_exc_get_type",
        llvm::FunctionType::get(i64Type, {}, false));
    // ptr dragon_exc_get_msg()
    getOrDeclareRuntime("dragon_exc_get_msg",
        llvm::FunctionType::get(i8PtrType, {}, false));
    // void dragon_raise_exc(i64 type, ptr msg)
    getOrDeclareRuntime("dragon_raise_exc",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    // dragon_raise_exc_cstr takes a raw rodata C-string; the runtime copies it
    // into a heap DragonString so exc_msg never holds a raw literal pointer.
    getOrDeclareRuntime("dragon_raise_exc_cstr",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    // dragon_raise_exc_obj: `raise UserExc(args)` hands the constructed instance
    // to the runtime so `except UserExc as e` binds the full instance, not just msg.
    getOrDeclareRuntime("dragon_raise_exc_obj",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType, i8PtrType}, false));
    // Consume variants take the message's owned +1 (concat/str()/f-string temps)
    // instead of dup'ing a borrow; see dragon_exc_msg_set in runtime_exception.cpp.
    getOrDeclareRuntime("dragon_raise_exc_consume",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    getOrDeclareRuntime("dragon_raise_exc_obj_consume",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType, i8PtrType}, false));
    // ptr dragon_exc_bind_msg() - `except ... as e` binding with its own +1.
    getOrDeclareRuntime("dragon_exc_bind_msg",
        llvm::FunctionType::get(i8PtrType, {}, false));
    // ptr dragon_exc_bind_obj() - instance binding with its own +1 (NULL-safe).
    getOrDeclareRuntime("dragon_exc_bind_obj",
        llvm::FunctionType::get(i8PtrType, {}, false));
    // ptr dragon_exc_retain_obj(ptr) - NULL-safe retain for deferred re-raise
    // saves and borrowed-instance raises (the consume raise transfers it back).
    getOrDeclareRuntime("dragon_exc_retain_obj",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // --- Unwind cleanup stack (frees owned heap locals a longjmp skips) ---
    auto* i32Ty = llvm::Type::getInt32Ty(*context);
    // i32 dragon_cleanup_push(i64 val, i32 kind, i32 tag)
    getOrDeclareRuntime("dragon_cleanup_push",
        llvm::FunctionType::get(i32Ty, {i64Type, i32Ty, i32Ty}, false));
    // void dragon_cleanup_update(i32 slot, i64 val, i32 tag)
    getOrDeclareRuntime("dragon_cleanup_update",
        llvm::FunctionType::get(voidType, {i32Ty, i64Type, i32Ty}, false));
    // i32 dragon_cleanup_depth()
    getOrDeclareRuntime("dragon_cleanup_depth",
        llvm::FunctionType::get(i32Ty, {}, false));
    // void dragon_cleanup_reset(i32 depth)
    getOrDeclareRuntime("dragon_cleanup_reset",
        llvm::FunctionType::get(voidType, {i32Ty}, false));
    // void dragon_exc_cleanup_unwind()
    getOrDeclareRuntime("dragon_exc_cleanup_unwind",
        llvm::FunctionType::get(voidType, {}, false));
    // ptr dragon_exc_get_obj() - read the in-flight instance; NULL when the
    // raise carried only a message.
    getOrDeclareRuntime("dragon_exc_get_obj",
        llvm::FunctionType::get(i8PtrType, {}, false));
    // void dragon_exc_register(i64 code, i64 parent_code)
    getOrDeclareRuntime("dragon_exc_register",
        llvm::FunctionType::get(voidType, {i64Type, i64Type}, false));
    // i64 dragon_exc_matches(i64 raised, i64 caught)
    getOrDeclareRuntime("dragon_exc_matches",
        llvm::FunctionType::get(i64Type, {i64Type, i64Type}, false));
    // dragon_vthread_log_uncaught: emitted by fire-trampoline's setjmp arrival to
    // log+clear an unhandled exception so the worker/parent loop survives it.
    getOrDeclareRuntime("dragon_vthread_log_uncaught",
        llvm::FunctionType::get(voidType, {}, false));
    // int setjmp(ptr env) -- returns_twice attribute
    {
        auto* setjmpType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(*context), {i8PtrType}, false);
        auto* setjmpFunc = getOrDeclareRuntime("setjmp", setjmpType);
        setjmpFunc->addFnAttr(llvm::Attribute::ReturnsTwice);
    }

    // --- Phase G: Builtin functions ---
    // G.1: Aggregate functions
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

    // G.2: Iteration helpers
    getOrDeclareRuntime("dragon_enumerate",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    getOrDeclareRuntime("dragon_zip",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_sorted",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ptr dragon_sorted_ex(ptr list, i64 reverse) - sorted(xs, reverse=...)
    getOrDeclareRuntime("dragon_sorted_ex",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));
    // void dragon_list_sort_ex(ptr list, i64 reverse) - list.sort(reverse=...)
    getOrDeclareRuntime("dragon_list_sort_ex",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // ptr dragon_list_concat(ptr a, ptr b) - list + list
    getOrDeclareRuntime("dragon_list_concat",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_reversed",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));

    // G.3: Type introspection
    getOrDeclareRuntime("dragon_hash_int",
        llvm::FunctionType::get(i64Type, {i64Type}, false));
    getOrDeclareRuntime("dragon_hash_str",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_id",
        llvm::FunctionType::get(i64Type, {i64Type}, false));

    // G.4: Numeric functions
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

    // --- Phase H: File I/O ---
    getOrDeclareRuntime("dragon_file_open",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));
    getOrDeclareRuntime("dragon_file_close",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_file_read",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_file_readline",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));

    // --- Bytes operations ---
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
    // Bytes conversions
    getOrDeclareRuntime("dragon_bytes_decode",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_str_encode",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // Bytes methods
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

    // --- OS Threading functions (thread {} scoped blocks) ---
    // ptr dragon_thread_fire(ptr fn, ptr args, i64 nargs)
    getOrDeclareRuntime("dragon_thread_fire",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type}, false));
    // i64 dragon_thread_join(ptr thread)
    getOrDeclareRuntime("dragon_thread_join",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));

    // --- Green thread functions (fire keyword -> M:N vthreads) ---
    // D030: ptr dragon_vthread_spawn_typed(ptr trampoline, ptr args, i64 args_size)
    getOrDeclareRuntime("dragon_vthread_spawn_typed",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type}, false));
    // D030: void dragon_vthread_set_result(ptr vt, i64 res)
    getOrDeclareRuntime("dragon_vthread_set_result",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // D030: ptr mco_get_user_data(ptr co) - minicoro API used inside codegen-emitted trampolines
    getOrDeclareRuntime("mco_get_user_data",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // D030: libc free() used by per-callsite spawn trampolines to release args buffer.
    getOrDeclareRuntime("free",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // i64 dragon_vthread_join(ptr vthread)
    getOrDeclareRuntime("dragon_vthread_join",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // void dragon_vthread_detach(ptr vthread) - drop the handle ref of a
    // discarded fire-and-forget vthread so it frees on completion (no leak).
    getOrDeclareRuntime("dragon_vthread_detach",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // i64 dragon_vthread_is_alive(ptr vthread)
    getOrDeclareRuntime("dragon_vthread_is_alive",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // --- Green thread I/O functions ---
    // void dragon_vthread_sleep(i64 ms)
    getOrDeclareRuntime("dragon_vthread_sleep",
        llvm::FunctionType::get(voidType, {i64Type}, false));
    // void dragon_vthread_yield()
    getOrDeclareRuntime("dragon_vthread_yield",
        llvm::FunctionType::get(voidType, {}, false));

    // --- Generator functions (coroutine-based lazy iteration) ---
    // D030: ptr dragon_generator_create_typed(ptr trampoline, ptr args, i64 args_size, ptr decref_fn)
    getOrDeclareRuntime("dragon_generator_create_typed",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type, i8PtrType}, false));
    // D030: void dragon_generator_set_exhausted(ptr gen)
    getOrDeclareRuntime("dragon_generator_set_exhausted",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_generator_set_raised(ptr gen) - trampoline barrier flag
    getOrDeclareRuntime("dragon_generator_set_raised",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_generator_yield(ptr gen, i64 value, i64 tag)
    getOrDeclareRuntime("dragon_generator_yield",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type, i64Type}, false));
    // i64 dragon_generator_next(ptr gen)
    getOrDeclareRuntime("dragon_generator_next",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // void dragon_generator_destroy(ptr gen)
    getOrDeclareRuntime("dragon_generator_destroy",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_generator_abandon(ptr gen) - reclaim a generator abandoned
    // mid-resume by a longjmp (its body raised); restores minicoro bookkeeping.
    getOrDeclareRuntime("dragon_generator_abandon",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    // --- OS thread functions (Thread class) ---
    // ptr dragon_osthread_new(ptr fn, ptr args, i64 nargs)
    getOrDeclareRuntime("dragon_osthread_new",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType, i64Type}, false));
    // i64 dragon_osthread_start(ptr handle)
    getOrDeclareRuntime("dragon_osthread_start",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // i64 dragon_osthread_join(ptr handle)
    getOrDeclareRuntime("dragon_osthread_join",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // i64 dragon_osthread_is_alive(ptr handle)
    getOrDeclareRuntime("dragon_osthread_is_alive",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));

    // --- Lock functions ---
    // ptr dragon_lock_new()
    getOrDeclareRuntime("dragon_lock_new",
        llvm::FunctionType::get(i8PtrType, {}, false));
    // void dragon_lock_acquire(ptr lock)
    getOrDeclareRuntime("dragon_lock_acquire",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // i64 dragon_lock_acquire_ex(ptr lock, i64 blocking, f64 timeout)
    // - acquire(blocking=..., timeout=...)
    getOrDeclareRuntime("dragon_lock_acquire_ex",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, f64Type}, false));
    // i64 dragon_lock_try_acquire(ptr lock)
    getOrDeclareRuntime("dragon_lock_try_acquire",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // void dragon_lock_release(ptr lock)
    getOrDeclareRuntime("dragon_lock_release",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_lock_destroy(ptr lock)
    getOrDeclareRuntime("dragon_lock_destroy",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // del debug tripwire (docs/002 ADR): assert the proven-sole-owner
    // refcount is exactly 1 at the del site. -O0 builds only.
    // void dragon_del_assert_unique(ptr p, i64 cls, ptr file, i64 line)
    getOrDeclareRuntime("dragon_del_assert_unique",
        llvm::FunctionType::get(voidType,
            {i8PtrType, i64Type, i8PtrType, i64Type}, false));
    // void dragon_del_assert_unique_box(i64 tag, i64 payload, ptr file, i64 line)
    getOrDeclareRuntime("dragon_del_assert_unique_box",
        llvm::FunctionType::get(voidType,
            {i64Type, i64Type, i8PtrType, i64Type}, false));
    // --- SyncList functions ---
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
    // --- SyncDict functions ---
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

    // --- GC reference counting ---
    // void dragon_incref(ptr obj)
    getOrDeclareRuntime("dragon_incref",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_decref(ptr obj)
    getOrDeclareRuntime("dragon_decref",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_incref_str(ptr data) -- string-specific (data -> header)
    getOrDeclareRuntime("dragon_incref_str",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_decref_str(ptr data) -- string-specific (data -> header)
    getOrDeclareRuntime("dragon_decref_str",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_str_make_immortal(ptr data) -- saturate a module-global const
    // string's refcount so a cross-worker-thread read never races on it.
    getOrDeclareRuntime("dragon_str_make_immortal",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // dragon_{incref,decref}_callable: tag-aware RC for Callable field slots
    // (bare fn ptr, no RC, vs DragonClosure with header/type_tag); mutates only the latter.
    getOrDeclareRuntime("dragon_incref_callable",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_decref_callable",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // ptr dragon_string_dup(ptr s) -- promote string literal to heap DragonString
    getOrDeclareRuntime("dragon_string_dup",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // dragon_str_retain is an identity incref; str(s)-of-a-str and single-part
    // f"{s}" route through it so results are owned +1 CallInsts.
    getOrDeclareRuntime("dragon_str_retain",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ptr dragon_exc_msg_preserve(ptr s) -- dup a re-raise message only if mortal heap
    getOrDeclareRuntime("dragon_exc_msg_preserve",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ptr dragon_str_intern(ptr utf8_bytes, i64 byte_len) -- one-shot UTF-8
    // decode + allocate + mark immortal. Used for non-ASCII string literals.
    getOrDeclareRuntime("dragon_str_intern",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i64Type}, false));

    // --- GC atomic reference counting (Phase 4: thread-safe) ---
    // void dragon_incref_atomic(ptr obj)
    getOrDeclareRuntime("dragon_incref_atomic",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_decref_atomic(ptr obj)
    getOrDeclareRuntime("dragon_decref_atomic",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_incref_str_atomic(ptr data) -- string-specific atomic
    getOrDeclareRuntime("dragon_incref_str_atomic",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_decref_str_atomic(ptr data) -- string-specific atomic
    getOrDeclareRuntime("dragon_decref_str_atomic",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));

    // --- GC SHARED-flag discrimination (D018 - vthread refcount race fix) ---
    // void dragon_mark_shared_deep(ptr obj) -- BFS-mark obj + reachable as SHARED
    getOrDeclareRuntime("dragon_mark_shared_deep",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_mark_shared(ptr obj) -- single-object mark
    getOrDeclareRuntime("dragon_mark_shared",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_mark_shared_str(ptr data)
    getOrDeclareRuntime("dragon_mark_shared_str",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_mark_shared_worklist_push(ptr worklist, ptr obj)
    getOrDeclareRuntime("dragon_mark_shared_worklist_push",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    // dragon_mark_shared_callable is tag-gated: a Callable field may be a bare
    // fn ptr (no header), which can't go through the raw worklist push.
    getOrDeclareRuntime("dragon_mark_shared_callable",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    // i64 dragon_class_register_mark_shared(i64 class_id, ptr fn)
    getOrDeclareRuntime("dragon_class_register_mark_shared",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    // dragon_mark_shared_boxed: tag-dispatched SHARED mark for a boxed Any/union
    // value stored in a module global (str, closure, and heap-tag cases).
    getOrDeclareRuntime("dragon_mark_shared_boxed",
        llvm::FunctionType::get(voidType, {i64Type, i64Type}, false));
    // void dragon_mark_shared_cell(ptr worklist, ptr cell) -- mark a captured
    // DragonCell + its held value (tag-dispatched via the cell's kind)
    getOrDeclareRuntime("dragon_mark_shared_cell",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));

    // --- GC Phase 5: cycle collector integration ---
    // i64 dragon_class_register_dealloc(ptr fn) -- returns class_id
    getOrDeclareRuntime("dragon_class_register_dealloc",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // i64 dragon_class_register_traverse(i64 class_id, ptr fn)
    getOrDeclareRuntime("dragon_class_register_traverse",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    // i64 dragon_class_register_clear(i64 class_id, ptr fn)
    getOrDeclareRuntime("dragon_class_register_clear",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    // void dragon_gc_track(ptr obj)
    getOrDeclareRuntime("dragon_gc_track",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // void dragon_gc_untrack(ptr obj)
    getOrDeclareRuntime("dragon_gc_untrack",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // i64 dragon_gc_collect()
    getOrDeclareRuntime("dragon_gc_collect",
        llvm::FunctionType::get(i64Type, {}, false));
    // void dragon_gc_set_threshold(i64 n)
    getOrDeclareRuntime("dragon_gc_set_threshold",
        llvm::FunctionType::get(voidType, {i64Type}, false));

    // --- Decision 025: First-class class descriptors ---
    // i64 dragon_class_descriptor_create(ptr name, i64 ctor, i64 class_id, i64 parent_desc, ptr doc)
    getOrDeclareRuntime("dragon_class_descriptor_create",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type, i64Type, i64Type, i8PtrType}, false));
    // ADR 025 removal: dragon_class_descriptor_call was deleted (no runtime
    // construction through a class value; construction is resolved statically).
    // i64 dragon_class_descriptor_get_name(i64 desc)
    getOrDeclareRuntime("dragon_class_descriptor_get_name",
        llvm::FunctionType::get(i64Type, {i64Type}, false));
    // ptr dragon_class_descriptor_get_doc(i64 desc) - niche-ptr Optional[str]
    getOrDeclareRuntime("dragon_class_descriptor_get_doc",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    // ptr dragon_instance_get_doc(ptr instance) - niche-ptr Optional[str]
    getOrDeclareRuntime("dragon_instance_get_doc",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ADR 025 removal: dragon_isinstance_runtime was deleted (isinstance is
    // resolved statically; the inheritance walk happens in codegen).

    // --- hasattr/getattr reflection ---
    // void dragon_class_descriptor_set_fields(i64 desc, ptr names, ptr offsets,
    //  ptr widths, i64 nfields)
    getOrDeclareRuntime("dragon_class_descriptor_set_fields",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType, i8PtrType, i8PtrType, i64Type}, false));
    // i64 dragon_hasattr(i64 instance, ptr attr_name)
    getOrDeclareRuntime("dragon_hasattr",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    // i64 dragon_getattr(i64 instance, ptr attr_name)
    getOrDeclareRuntime("dragon_getattr",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    // i64 dragon_getattr_default(i64 instance, ptr attr_name, i64 default_val)
    getOrDeclareRuntime("dragon_getattr_default",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType, i64Type}, false));

    // --- D033: method-name reflection (drives dir() and getattr() bind) ---
    // void dragon_class_descriptor_set_methods(i64 desc, ptr names, ptr fn_ptrs,
    //  ptr kinds, i64 nmethods)
    getOrDeclareRuntime("dragon_class_descriptor_set_methods",
        llvm::FunctionType::get(voidType,
            {i64Type, i8PtrType, i8PtrType, i8PtrType, i64Type}, false));
    // ptr dragon_class_find_method(i64 desc, ptr name) - walks parent chain.
    getOrDeclareRuntime("dragon_class_find_method",
        llvm::FunctionType::get(i8PtrType, {i64Type, i8PtrType}, false));
    // i64 dragon_class_find_method_kind(i64 desc, ptr name) - -1 if absent.
    getOrDeclareRuntime("dragon_class_find_method_kind",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    // ptr dragon_dir(i64 instance_or_desc, i64 is_descriptor) - returns
    // a list[str] of attribute names; powers the dir() builtin.
    getOrDeclareRuntime("dragon_dir",
        llvm::FunctionType::get(i8PtrType, {i64Type, i64Type}, false));
    // void dragon_class_descriptor_set_method_bound_thunks(i64 desc, ptr thunks)
    getOrDeclareRuntime("dragon_class_descriptor_set_method_bound_thunks",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    // ptr dragon_class_find_method_bound(i64 desc, ptr name)
    getOrDeclareRuntime("dragon_class_find_method_bound",
        llvm::FunctionType::get(i8PtrType, {i64Type, i8PtrType}, false));

    // --- D027/D030: Closure and environment functions ---
    // ptr dragon_env_alloc(i64 total_size, ptr gc_fn, i32 trackable)
    //  Allocates header+body (layout owned by codegen). gc_fn is the multi-op
    //  env GC hook; trackable=1 gc-tracks the env so a capture cycle is collectable.
    getOrDeclareRuntime("dragon_env_alloc",
        llvm::FunctionType::get(i8PtrType,
            {i64Type, i8PtrType, llvm::Type::getInt32Ty(*context)}, false));
    // ptr dragon_closure_create(ptr fn, ptr env)
    //  Closure fields are accessed via inline GEPs in codegen - no get_fn/get_env runtime calls.
    getOrDeclareRuntime("dragon_closure_create",
        llvm::FunctionType::get(i8PtrType, {i8PtrType, i8PtrType}, false));

    // --- Heap-boxed mutable cells (`nonlocal` storage) ---
    // ptr dragon_cell_alloc(i64 init_value, i32 kind, i32 holds_heap)
    getOrDeclareRuntime("dragon_cell_alloc",
        llvm::FunctionType::get(i8PtrType,
            {i64Type, llvm::Type::getInt32Ty(*context),
             llvm::Type::getInt32Ty(*context)}, false));
    // i64 dragon_cell_get(ptr cell)
    getOrDeclareRuntime("dragon_cell_get",
        llvm::FunctionType::get(i64Type, {i8PtrType}, false));
    // i64 dragon_cell_set(ptr cell, i64 new_value) // returns old value for caller decref
    getOrDeclareRuntime("dragon_cell_set",
        llvm::FunctionType::get(i64Type, {i8PtrType, i64Type}, false));

    // --- Template escape functions (pipe filters) ---
    // ptr dragon_template_escape_html(ptr s) -> ptr
    getOrDeclareRuntime("dragon_template_escape_html",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ptr dragon_template_escape_sql(ptr s) -> ptr
    getOrDeclareRuntime("dragon_template_escape_sql",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ptr dragon_template_escape_url(ptr s) -> ptr
    getOrDeclareRuntime("dragon_template_escape_url",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
}

// Forward-declare all top-level functions in a module
void CodeGen::Impl::forwardDeclareFunctions(dragon::Module& mod) {
    for (auto& stmt : mod.body) {
        if (auto* func = dynamic_cast<FunctionDecl*>(stmt.get())) {
            // D044 - never forward-declare a generic template; only its stamped
            // monomorphic instantiations (empty typeParams) get LLVM symbols.
            if (!func->typeParams.empty()) continue;
            // Extern-C decls keep the bare C-ABI name so the linker resolves them;
            // same-module Dragon defs get mangled. `as DRAGON_NAME` aliases the two via externSymbol.
            const std::string externLinkName =
                func->externSymbol.empty() ? func->name : func->externSymbol;
            const std::string llvmName = func->isExtern
                ? userFuncName(externLinkName)
                : mangleFunc(currentModuleName, func->name);
            if (module->getFunction(llvmName)) {
                // Even with an existing LLVM symbol (shared C symbol or pre-declared
                // dragon_* fn), its alias and RC-side maps must still be registered here.
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
                continue; // already declared
            }
            std::vector<llvm::Type*> paramTypes;
            std::vector<bool> tagMask;  // union tag param tracking
            VarArgInfo vaInfo;
            bool seenVarArg = false;
            for (auto& p : func->params) {
                if (p.isVarArg) {
                    seenVarArg = true;
                    if (!p.name.empty()) {
                        // *args -> i8* (list pointer)
                        vaInfo.hasVarArg = true;
                        vaInfo.varArgName = p.name;
                        // Derive the element repr from `*args: T`: the call site packs
                        // into the matching monomorphized list variant; no annotation defaults to tag 0 (i64).
                        if (p.type) {
                            Type::Kind tk =
                                elemVarKindToTypeKind(typeExprToKind(p.type.get()));
                            vaInfo.varArgElemTag = typeKindToElemTag(tk);
                            vaInfo.varArgElemIsAny = (tk == Type::Kind::Any);
                        }
                        paramTypes.push_back(i8PtrType);
                        tagMask.push_back(false);
                    }
                    // bare * separator: skip, no LLVM param
                    continue;
                }
                if (p.isKwArg) {
                    // **kwargs -> i8* (dict pointer)
                    vaInfo.hasKwArg = true;
                    vaInfo.kwArgName = p.name;
                    paramTypes.push_back(i8PtrType);
                    tagMask.push_back(false);
                    continue;
                }
                if (!seenVarArg)
                    vaInfo.numRegularParams++;
                // D030 Phase 4: a union param is a single {i64,i64} box, no
                // trailing tag arg; typeExprToLLVM(UnionTypeExpr) returns boxType.
                paramTypes.push_back(typeExprToLLVM(p.type.get()));
                tagMask.push_back(false);
            }
            // Side maps key by the LLVM symbol (post-mangling), not the bare name,
            // so two modules with same-named `def open` keep distinct metadata entries.
            if (vaInfo.hasVarArg || vaInfo.hasKwArg)
                funcVarArgInfo[llvmName] = vaInfo;
            llvm::Type* retType;
            if (func->isExtern && !func->returnType) {
                retType = voidType;  // extern C: no annotation = void
            } else if (func->isAsync) {
                retType = i8PtrType; // async def returns vthread handle (Task)
            } else if (containsYield(func->body)) {
                retType = i8PtrType; // generator function returns generator object
                generatorFunctions.insert(llvmName);
            } else if (!func->returnType) {
                // No annotation means void unless the body returns a value
                // (historical int default); must match Functions.cpp's body emission or LLVM verify fails.
                retType = unannotatedReturnType(func->body);
            } else {
                retType = typeExprToLLVM(func->returnType.get());
            }
            // D027: this pre-pass runs before any body emission, so a method
            // calling a closure factory sees it already marked. Keyed by the bare Dragon name.
            if (functionReturnsClosure(*func))
                funcReturnsClosure.insert(llvmName);
            // D027: records which params are Callable[...] so a bare fn passed
            // there gets wrapped into DragonClosure(fn, null); indexed by AST param position.
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
            // GC Phase 4: store param VarKinds for atomic incref at fire/async spawn. A
            // union param is a single box arg - one VarKind slot, no tag-arg companion.
            if (options.gcMode == GCMode::RC) {
                std::vector<VarKind> pkinds;
                std::vector<bool> powns;
                for (auto& p : func->params) {
                    pkinds.push_back(typeExprToKind(p.type.get()));
                    powns.push_back(p.isOwn);
                }
                funcParamKinds[llvmName] = std::move(pkinds);
                funcParamOwns[llvmName] = std::move(powns);
                // Extern "C" callees follow the FFI v0 contract (externDrainableFuncs):
                // args borrowed, returns fresh unless `ptr`-typed (may alias an arg).
                if (func->isExtern) {
                    externFuncNames.insert(llvmName);
                    bool ptrReturn = false;
                    if (auto* rn = dynamic_cast<NamedTypeExpr*>(func->returnType.get()))
                        ptrReturn = (rn->name == "ptr");
                    if (!ptrReturn) externDrainableFuncs.insert(llvmName);
                }
            }
            // Store default parameter values for call-site filling
            {
                std::vector<Expr*> defaults;
                for (auto& p : func->params)
                    defaults.push_back(p.defaultValue.get());
                funcParamDefaults[llvmName] = std::move(defaults);
                // Record which module owns these defaults so fillDefaultArgs
                // can eval them with the right module-private symbol scope.
                funcDefiningModule[llvmName] = currentModuleName;
            }
            // D040: declared parameter names for keyword-arg binding; vararg/kwarg
            // slots use their declared name ("" for the bare `*` separator).
            {
                std::vector<std::string> names;
                for (auto& p : func->params)
                    names.push_back(p.name);
                funcParamNames[llvmName] = std::move(names);
            }
            // Tracks functions returning a class instance so resolveExprClassName
            // can resolve `make_box(...).method()` chains without a named receiver var.
            if (auto* retNamed = dynamic_cast<NamedTypeExpr*>(func->returnType.get())) {
                if (classNames.count(retNamed->name))
                    funcReturnClassNames[llvmName] = retNamed->name;
            } else if (func->returnType) {
                // D044 - a function returning a generic instantiation (`-> Box[int]`)
                // registers the stamped class too, so `mk().method()` chains dispatch.
                std::string gn = genericInstanceClassName(func->returnType.get());
                if (!gn.empty()) funcReturnClassNames[llvmName] = gn;
            }
            // Collect extern library hints for the linker
            if (func->isExtern && !func->externLib.empty()) {
                externLibs.insert(func->externLib);
            }
            // Heuristics inspect the C symbol, not the (possibly aliased) Dragon
            // name; for a plain extern these are the same string, for an alias they diverge.
            // Auto-detect sqlite3 usage for bundled lib linking
            if (func->isExtern && externLinkName.substr(0, 7) == "sqlite3") {
                needsSqlite3 = true;
            }
            // Auto-detect PCRE2 usage for bundled lib linking
            if (func->isExtern && externLinkName.substr(0, 5) == "pcre2") {
                needsPcre2 = true;
            }
            // Auto-detect mbedTLS: both the dragon_tls_* shim and the crypto
            // digests/HMAC (dragon_sha*/md5*/hmac*, ADR 038 Phase 7) need libdragon_mbedtls.a linked.
            if (func->isExtern &&
                (externLinkName.substr(0, 10) == "dragon_tls" ||
                 externLinkName.substr(0, 10) == "dragon_sha" ||
                 externLinkName.substr(0, 10) == "dragon_md5" ||
                 externLinkName.substr(0, 11) == "dragon_hmac")) {
                needsMbedtls = true;
            }
            // Auto-detect zlib/zstd: any extern ref to dragon_zlib_*/dragon_zstd_*
            // (gzip.dr, zstandard.dr, tarfile.dr, or user code) means the link needs -lz/-lzstd.
            if (func->isExtern && externLinkName.substr(0, 11) == "dragon_zlib") {
                needsZ = true;
            }
            if (func->isExtern && externLinkName.substr(0, 11) == "dragon_zstd") {
                needsZstd = true;
            }
            // Auto-detect the ui module's webview shell (D031): not part of the
            // runtime archive, so linkExecutable compiles it in per-app and links webkit2gtk.
            if (func->isExtern &&
                externLinkName.substr(0, 14) == "dragon_webview") {
                needsWebview = true;
            }
            // An aliased extern registers `name -> llvmName` in the module's alias
            // scope so a Dragon call to the alias resolves to the C symbol.
            if (func->isExtern && !func->externSymbol.empty()) {
                importedFuncAliasesByModule[currentModuleName][func->name] = llvmName;
            }
            (void)llvmFunc;
        }
    }
}

// Forward-declares class constructors/methods. N>1 __init__ overloads mangle
// as ClassName___init___0.._N-1 / _new_0.._N-1; N<=1 keeps the un-suffixed names.
void CodeGen::Impl::forwardDeclareClasses(dragon::Module& mod) {
    // Synthesizes @dataclass/NamedTuple/enum methods before scanning __init__, so
    // forwardDeclareClasses sees them; enum runs first so dataclass sees a plain class.
    for (auto& stmt : mod.body) {
        if (auto* classDecl = dynamic_cast<ClassDecl*>(stmt.get())) {
            if (!classDecl->typeParams.empty()) continue;  // D044 - template, not lowered
            synthesizeEnumMethods(*classDecl);
            synthesizeDataclassMethods(*classDecl);
        }
    }
    // First pass: register ALL class names so cross-references resolve.
    // TypedDict classes go into typedDictClasses instead of classNames.
    for (auto& stmt : mod.body) {
        if (auto* classDecl = dynamic_cast<ClassDecl*>(stmt.get())) {
            if (!classDecl->typeParams.empty()) continue;  // D044 - template, not lowered
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
                // Collect field schemas
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
                // Tracks which module owns this class for `<mod>__<class>_new` symbol
                // lookup; last-write-wins is fine since resolveClassOwningModule tries same-module first.
                // D044 cross-module generics: a stamped instantiation lives in the
                // instantiating module but is OWNED by the template's defining module.
                classOwningModule[classDecl->name] =
                    classDecl->genericHomeModule.empty()
                        ? currentModuleName
                        : classDecl->genericHomeModule;
            }
        }
    }
    // Second pass: create struct types, init/new, and methods (skip TypedDict)
    for (auto& stmt : mod.body) {
        if (auto* classDecl = dynamic_cast<ClassDecl*>(stmt.get())) {
            if (!classDecl->typeParams.empty()) continue;  // D044 - template, not lowered
            // Metadata key: home-module-aware, identical to the visit(ClassDecl) key.
            const std::string csym = mangleClass(
                classDecl->genericHomeModule.empty() ? currentModuleName
                                                     : classDecl->genericHomeModule,
                classDecl->name);
            if (typedDictClassesBySym.count(csym)) continue;

            // D044 cross-module generics: forward-declares a stamped instantiation's
            // symbols under the template's defining module, restored at iteration end.
            std::string _savedMod = currentModuleName;
            struct RM { std::string* s; std::string v; bool a; ~RM(){ if (a) *s = v; } }
                _rm{&currentModuleName, _savedMod,
                    !classDecl->genericHomeModule.empty()};
            if (!classDecl->genericHomeModule.empty())
                currentModuleName = classDecl->genericHomeModule;

            // Track parent for MRO: bare or dotted base, stored as the parent's
            // SYM (resolved alias-aware in this module's context).
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

                    // Detect user-defined exception classes
                    if (isExcType(baseBareName)) {
                        int64_t code = userExcNextCode++;
                        userExcCodesBySym[csym] = code;
                        userExcParentCodes[code] = excTypeCode(baseBareName);
                    }
                }
            }

            // Collect ALL __init__ FunctionDecls
            std::vector<FunctionDecl*> initDecls;
            for (auto& classStmt : classDecl->body) {
                if (auto* fd = dynamic_cast<FunctionDecl*>(classStmt.get())) {
                    if (fd->name == "__init__") initDecls.push_back(fd);
                }
            }

            size_t ctorCount = initDecls.size();

            // Per-module class symbol prefix, mirrors mangleFunc: two modules with
            // same-named classes get distinct LLVM symbols.
            const std::string clsSym = mangleClass(currentModuleName, classDecl->name);

            if (ctorCount == 0) {
                // No explicit constructor: synthesizes a zero-arg __init__/_new
                // (Python parity for `class Foo: pass`); the body calls the parent's zero-arg ctor if present.
                std::string initName = clsSym + "___init__";
                if (!module->getFunction(initName)) {
                    auto* initFuncType = llvm::FunctionType::get(
                        voidType, {i8PtrType}, false);  // just self
                    llvm::Function::Create(initFuncType, llvm::Function::InternalLinkage,
                                           initName, module.get());
                }
                std::string newName = clsSym + "_new";
                if (!module->getFunction(newName)) {
                    auto* newFuncType = llvm::FunctionType::get(i8PtrType, {}, false);
                    llvm::Function::Create(newFuncType, llvm::Function::InternalLinkage,
                                           newName, module.get());
                }
                // Zero-arg ctor: registers EMPTY param metadata since a missing
                // entry is treated as a compiler invariant violation (CallExpr.cpp backstop).
                if (options.gcMode == GCMode::RC) {
                    funcParamKinds[newName] = {};
                    funcParamOwns[newName] = {};
                }
            } else if (ctorCount == 1) {
                // --- Single-constructor path ---
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

                // Store default parameter values for _new (indexed by LLVM param position)
                {
                    std::vector<Expr*> defaults;
                    for (size_t i = paramStart; i < initDecl->params.size(); ++i) {
                        defaults.push_back(initDecl->params[i].defaultValue.get());
                    }
                    funcParamDefaults[newName] = std::move(defaults);
                    // Record defining module so fillDefaultArgs resolves the
                    // expressions with the ctor module's symbol scope.
                    funcDefiningModule[newName] = currentModuleName;
                }
                // D040: parameter names for constructor keyword-arg binding.
                {
                    std::vector<std::string> names;
                    for (size_t i = paramStart; i < initDecl->params.size(); ++i) {
                        names.push_back(initDecl->params[i].name);
                    }
                    funcParamNames[newName] = std::move(names);
                }
            } else {
                // --- Multi-constructor path ---
                classCtorCountBySym[csym] = ctorCount;
                auto& arityVec = classCtorAritiesBySym[csym];
                arityVec.clear();

                for (size_t ci = 0; ci < ctorCount; ++ci) {
                    FunctionDecl* fd = initDecls[ci];
                    int ctorIdx = fd->constructorIndex >= 0 ? fd->constructorIndex : (int)ci;

                    // Compute arity (number of user-visible params, excluding self)
                    size_t paramStart = fd->hasImplicitSelf ? 0 : 1;
                    size_t arity = fd->params.size() - paramStart;
                    arityVec.push_back({arity, ctorIdx});

                    // Build param type lists
                    std::vector<llvm::Type*> initParamTypes = {i8PtrType}; // self
                    std::vector<llvm::Type*> newParamTypes;
                    for (size_t i = paramStart; i < fd->params.size(); ++i) {
                        llvm::Type* pt = typeExprToLLVM(fd->params[i].type.get());
                        initParamTypes.push_back(pt);
                        newParamTypes.push_back(pt);
                    }

                    // Forward-declare <mod>__<cls>___init___N
                    std::string initName = clsSym + "___init___" + std::to_string(ctorIdx);
                    if (!module->getFunction(initName)) {
                        auto* initFuncType = llvm::FunctionType::get(voidType, initParamTypes, false);
                        llvm::Function::Create(initFuncType, llvm::Function::InternalLinkage,
                                               initName, module.get());
                    }

                    // Forward-declare <mod>__<cls>_new_N
                    std::string newName = clsSym + "_new_" + std::to_string(ctorIdx);
                    if (!module->getFunction(newName)) {
                        auto* newFuncType = llvm::FunctionType::get(i8PtrType, newParamTypes, false);
                        llvm::Function::Create(newFuncType, llvm::Function::InternalLinkage,
                                               newName, module.get());
                    }

                    // Same forward registration as the single-ctor path
                    // (fire-own-fwdref-hang.md): param kinds/own flags must be visible before any body lowers.
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

            // Forward-declare regular methods and track dunder methods
            for (auto& classStmt : classDecl->body) {
                auto* methodDecl = dynamic_cast<FunctionDecl*>(classStmt.get());
                if (!methodDecl || methodDecl->name == "__init__") continue;
                // D044+: skips a generic-method template (own type param); its
                // concrete stamps are appended to this body by the monomorphizer.
                if (!methodDecl->typeParams.empty()) continue;

                // D045: gates on the shared isReservedDunder allowlist so the dispatch
                // table and predicate share one source of truth (unrecognized __x__ is already an error).
                if (isReservedDunder(methodDecl->name)) {
                    classDunderMethodsBySym[csym].insert(methodDecl->name);
                }

                std::string methodName = clsSym + "_" + methodDecl->name;
                // ADR 010: an overloaded method (>1 decl with this name) gets a
                // per-index symbol; the call site appends the same `__ovN`.
                if (methodDecl->methodOverloadCount > 1 &&
                    methodDecl->methodOverloadIndex >= 0)
                    methodName += "__ov" + std::to_string(methodDecl->methodOverloadIndex);
                if (module->getFunction(methodName)) continue;

                // Static/@classmethod do NOT receive self as first parameter
                std::vector<llvm::Type*> methodParamTypes;
                if (!methodDecl->isStatic) {
                    methodParamTypes.push_back(i8PtrType); // self
                } else {
                    staticMethods.insert(methodName);
                }
                // Where user params start: classmethod skips cls (1); static/implicit-self
                // start at 0; explicit self (.py instance) skips self (1).
                size_t mParamStart;
                if (methodDecl->isClassMethod) {
                    mParamStart = 1;  // skip cls
                } else if (methodDecl->isStatic || methodDecl->hasImplicitSelf) {
                    mParamStart = 0;
                } else {
                    mParamStart = 1;  // skip explicit self
                }
                // `*args`/`**kwargs` collapse to one i8* like a variadic free function
                // (VarArgInfo drives call-site packing); the bare `*` separator has no LLVM param.
                VarArgInfo vaInfo;
                bool seenVarArg = false;
                for (size_t i = mParamStart; i < methodDecl->params.size(); ++i) {
                    const auto& p = methodDecl->params[i];
                    if (p.isVarArg) {
                        seenVarArg = true;
                        if (p.name.empty()) continue;  // bare * separator
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

                // A method with `yield` is a generator: the fn is the WRAPPER returning
                // the generator object. Instance/static generators work; @classmethod ones don't yet.
                bool methodIsGenerator =
                    containsYield(methodDecl->body) && !methodDecl->isClassMethod;
                llvm::Type* retType =
                    methodIsGenerator ? i8PtrType
                    : (methodDecl->returnType
                        ? typeExprToLLVM(methodDecl->returnType.get())
                        : unannotatedReturnType(methodDecl->body));
                auto* methodFuncType = llvm::FunctionType::get(retType, methodParamTypes, false);
                llvm::Function::Create(methodFuncType, llvm::Function::InternalLinkage,
                                       methodName, module.get());
                // Registers variadic metadata under the post-mangling/post-ovN symbol
                // so the call site packs surplus positionals/keywords into *args/**kwargs.
                if (vaInfo.hasVarArg || vaInfo.hasKwArg)
                    funcVarArgInfo[methodName] = vaInfo;
                if (methodIsGenerator) {
                    generatorFunctions.insert(methodName);
                    generatorYieldKinds[methodName] = inferYieldKind(methodDecl->body);
                }

                // D018 fix: stores method param VarKinds so `fire self.method(...)`
                // emits the right atomic-incref+mark-shared per arg (else Router state raced, crashing GC).
                if (options.gcMode == GCMode::RC) {
                    std::vector<VarKind> mkinds;
                    std::vector<bool> mowns;
                    if (!methodDecl->isStatic) {
                        // self is a ClassInstance heap arg.
                        mkinds.push_back(VarKind::ClassInstance);
                        mowns.push_back(false);
                    }
                    for (size_t i = mParamStart; i < methodDecl->params.size(); ++i) {
                        const auto& p = methodDecl->params[i];
                        if (p.isVarArg && p.name.empty()) continue;  // bare *
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

                // Store default parameter values (indexed by LLVM param position)
                {
                    std::vector<Expr*> defaults;
                    if (!methodDecl->isStatic) {
                        defaults.push_back(nullptr); // self has no default
                    }
                    for (size_t i = mParamStart; i < methodDecl->params.size(); ++i) {
                        const auto& p = methodDecl->params[i];
                        if (p.isVarArg && p.name.empty()) continue;  // bare *
                        // *args/**kwargs have no default; the pack is always
                        // synthesized at the call site.
                        if (p.isVarArg || p.isKwArg) {
                            defaults.push_back(nullptr);
                            continue;
                        }
                        defaults.push_back(p.defaultValue.get());
                    }
                    funcParamDefaults[methodName] = std::move(defaults);
                    // Record defining module so cross-module method calls
                    // evaluate defaults under the owning module's scope.
                    funcDefiningModule[methodName] = currentModuleName;
                }
                // D040: parameter names for method keyword-arg binding; self gets
                // "self" (never a valid kwarg) to keep the vector aligned to the LLVM param count.
                {
                    std::vector<std::string> names;
                    if (!methodDecl->isStatic) {
                        names.push_back("self");
                    }
                    for (size_t i = mParamStart; i < methodDecl->params.size(); ++i) {
                        const auto& p = methodDecl->params[i];
                        if (p.isVarArg && p.name.empty()) continue;  // bare *
                        names.push_back(p.name);
                    }
                    funcParamNames[methodName] = std::move(names);
                }

                // Track methods that return class instances (for cross-module dispatch)
                if (auto* retNamed = dynamic_cast<NamedTypeExpr*>(methodDecl->returnType.get())) {
                    if (classNames.count(retNamed->name))
                        methodReturnClassNames[methodName] = retNamed->name;
                }
                // Tracks the declared return Type::Kind so callers pick the right
                // VarKind for a returned ptr value; drives ForLoop.cpp's __next__-binding path.
                if (methodDecl->returnType)
                    methodReturnKinds[methodName] =
                        typeExprToTypeKind(methodDecl->returnType.get());

                // @property: registers getter/setter metadata; setters are parser-mangled
                // to "<propName>__setter" so they get their own vtable slot.
                if (methodDecl->isProperty) {
                    classPropertiesBySym[csym].insert(methodDecl->name);
                }
                if (!methodDecl->propertySetterFor.empty()) {
                    classPropertySettersBySym[csym][methodDecl->propertySetterFor] =
                        methodDecl->name; // already mangled "<prop>__setter"
                }
            }

            // Decision 026: builds vtable method order (parent + overrides). D033:
            // also builds classOwnMethods (this class's own, in decl order) for dir()/find_method.
            {
                std::vector<std::string> vtableOrder;
                std::vector<std::string> ownMethods;
                auto parentIt = classParentNamesBySym.find(csym);
                if (parentIt != classParentNamesBySym.end()) {
                    auto poIt = classVtableMethodOrderBySym.find(parentIt->second);
                    if (poIt != classVtableMethodOrderBySym.end())
                        vtableOrder = poIt->second; // inherit parent order
                }
                for (auto& classStmt : classDecl->body) {
                    auto* md = dynamic_cast<FunctionDecl*>(classStmt.get());
                    if (!md) continue;
                    // D033: record kind even for __init__; consumers decide
                    // whether to surface it via dir() / find_method.
                    uint8_t kind = 0;
                    if (md->isClassMethod) kind = 2;
                    else if (md->isStatic) kind = 1;
                    classMethodKindsBySym[csym][md->name] = kind;
                    if (md->name == "__init__") continue;
                    ownMethods.push_back(md->name);
                    // Check if already inherited (override -- keep same index)
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

    // 1. Conformance sets from the TypeChecker's stamps, closed over inheritance:
    // a subclass fills the same colored slots so MRO override resolution wins.
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

    // 2. Color: sequential indices past the largest natural vtable, so a colored
    // slot never collides with a class's own method ordinals (deterministic order).
    unsigned base = 0;
    for (auto& [csym, order] : classVtableMethodOrderBySym)
        base = std::max(base, (unsigned)order.size());
    unsigned next = base;
    for (auto* cd : contractDeclsInOrder)
        for (auto& m : cd->methods)
            contractMethodSlots[{cd, m->name}] = next++;

    // 3. Extends each conforming class's vtable order; unused indices pad with ""
    // (null ptr), used ones get the method name for MRO resolution to fill in.
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

} // namespace dragon
