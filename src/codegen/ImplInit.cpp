/// Dragon CodeGen - Impl initialization, runtime declarations, and forward declarations.
/// Extracted from CodeGenImpl.h to reduce header size.
#include "../CodeGenImpl.h"
#include "dragon/Privacy.h"

namespace dragon {

void CodeGen::Impl::init() {
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("dragon_module", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    // Set target triple
    if (options.targetTriple.empty()) {
        options.targetTriple = llvm::sys::getDefaultTargetTriple();
    }
    module->setTargetTriple(llvm::Triple(options.targetTriple));

    // Set the target DataLayout NOW, at module creation, not just before
    // compileToObject. Otherwise every getStructLayout()/getTypeAllocSize()
    // call during IR building sees LLVM's default layout (which, for example,
    // aligns i64 to 4 bytes), while the final object is lowered with the target
    // layout (i64 aligned to 8). Any numeric offset/size baked into a constant
    // during IR building - e.g. the class field byte-offsets emitted for
    // getattr() reflection - would then disagree with where fields actually
    // land, returning shifted/garbage values. Looking up the target may fail in
    // odd toolchains; fall back silently to the default (still better than a
    // mismatch only for exotic targets without a registered backend).
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

    // Cache common types
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

    // TBAA metadata tree for alias analysis.
    // Tells LLVM that list struct fields (data ptr, size) don't alias
    // with list element array data, enabling LICM in loops.
    tbaaRoot = llvm::MDNode::get(*context, {
        llvm::MDString::get(*context, "Dragon TBAA")});
    tbaaListHeader = llvm::MDNode::get(*context, {
        llvm::MDString::get(*context, "list header"), tbaaRoot,
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Type, 0))});
    tbaaListData = llvm::MDNode::get(*context, {
        llvm::MDString::get(*context, "list data"), tbaaRoot,
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i64Type, 0))});

    // Push global scope
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
    // D039 Phase 2: %dragon.box dragon_dict_get_box(ptr dict, ptr key)
    // Returns {i64 tag, i64 payload} by value (2 registers on AMD64).
    // Borrow-return contract - caller increfs the payload by tag when storing
    // into a longer-lived slot.
    getOrDeclareRuntime("dragon_dict_get_box",
        llvm::FunctionType::get(boxType, {i8PtrType, i8PtrType}, false));
    // D039 Phase 3: void dragon_print_box(%dragon.box)
    // Tag-dispatched print for Any/Union/Optional values.
    getOrDeclareRuntime("dragon_print_box",
        llvm::FunctionType::get(voidType, {boxType}, false));

    // D039 Phase 4: list[Any] runtime ops - per-element {tag, payload}
    // storage. Matches Go's []interface{} speed model: 16 bytes/elem, single
    // cache miss per read.
    // ptr dragon_list_box_new(i64 capacity)
    getOrDeclareRuntime("dragon_list_box_new",
        llvm::FunctionType::get(i8PtrType, {i64Type}, false));
    // %dragon.box dragon_list_box_get(ptr list, i64 index)
    getOrDeclareRuntime("dragon_list_box_get",
        llvm::FunctionType::get(boxType, {i8PtrType, i64Type}, false));
    // void dragon_list_view_check(ptr list, i64 want_elem_tag) - raises
    // TypeError when a box-tagged list payload's representation (DragonList
    // vs DragonListBox, elem_tag) doesn't match the typed view unboxing it.
    getOrDeclareRuntime("dragon_list_view_check",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
    // i64 dragon_box_len(%dragon.box) - len() of an Any value, tag + header
    // dispatched (str/list-of-either-representation/dict/bytes); raises the
    // Python-shaped TypeError for unsized values.
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
    // dragon_box_subscript(box container, box index) - `anyVal[i]`.
    // Tag-dispatched element read for an Any-boxed list/dict/str/bytes; returns
    // an OWNED box (borrowed container elements are incref'd, mirroring the
    // owned-str convention so transient results can be released).
    getOrDeclareRuntime("dragon_box_subscript",
        llvm::FunctionType::get(boxType, {boxType, boxType}, false));
    // void dragon_box_decref(box) - release an owned box temporary's payload by
    // tag (no-op for non-refcounted tags). Frees box_binop / box_subscript
    // results after a transient use (print / discarded statement).
    getOrDeclareRuntime("dragon_box_decref",
        llvm::FunctionType::get(voidType, {boxType}, false));

    // unittest support: deep container equality. dragon_box_eq recurses
    // through these, and codegen calls them directly for `list == list` /
    // `dict == dict` when neither side is box-typed (faster than boxing).
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
    // ptr dragon_deque_popleft_ptr(ptr deque) / dragon_deque_pop_ptr(ptr deque)
    // Heap-element pop variants: same transfer semantics, ptr return so the
    // result is recognized as an OWNED ptr (drained when discarded / passed to a
    // borrow callee, adopted when bound). Mirrors dragon_dict_get_ptr (#19).
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
    // void dragon_raise_exc_cstr(i64 type, ptr msg) - raw C-string message
    // (rodata literal): the runtime copies it into a heap DragonString so the
    // exc_msg slot never holds a raw literal pointer (no OOB header probe).
    getOrDeclareRuntime("dragon_raise_exc_cstr",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType}, false));
    // void dragon_raise_exc_obj(i64 type, ptr obj, ptr msg) - typed-field
    // exception raise: `raise UserExc(args)` constructs the instance, then
    // hands it to the runtime so the matching `except UserExc as e` handler
    // binds `e` to the full instance (typed fields intact), not just `msg`.
    getOrDeclareRuntime("dragon_raise_exc_obj",
        llvm::FunctionType::get(voidType, {i64Type, i8PtrType, i8PtrType}, false));
    // Consume variants: the slot takes the message's owned +1 (concat /
    // str() / f-string temporaries, retained finally re-raise) instead of
    // dup'ing a borrow. See dragon_exc_msg_set in runtime_exception.cpp.
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
    // void dragon_vthread_log_uncaught() - emitted by fire-trampoline's
    // setjmp-arrival branch to log + clear an unhandled exception, so the
    // worker thread / parent accept loop survives the failed vthread.
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
    // void dragon_generator_yield(ptr gen, i64 value)
    getOrDeclareRuntime("dragon_generator_yield",
        llvm::FunctionType::get(voidType, {i8PtrType, i64Type}, false));
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
    // void dragon_incref_callable(ptr) / dragon_decref_callable(ptr)
    //  Tag-aware RC for `Callable[[...], R]` field slots: a Callable
    //  field can hold either a bare LLVM fn pointer (no header, no RC) or
    //  a DragonClosure* (header at offset 0, type_tag at offset 8). The
    //  helper inspects type_tag and only mutates refcount when the value
    //  is a DragonClosure. Codegen emits these for class-field stores
    //  declared as `Callable[...]`.
    getOrDeclareRuntime("dragon_incref_callable",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    getOrDeclareRuntime("dragon_decref_callable",
        llvm::FunctionType::get(voidType, {i8PtrType}, false));
    // ptr dragon_string_dup(ptr s) -- promote string literal to heap DragonString
    getOrDeclareRuntime("dragon_string_dup",
        llvm::FunctionType::get(i8PtrType, {i8PtrType}, false));
    // ptr dragon_str_retain(ptr s) -- identity incref. str(s)-of-a-str and
    // single-part f"{s}" route through this so their results are owned +1
    // CallInsts (the calls-return-owned convention consumers assume).
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
    // void dragon_mark_shared_callable(ptr worklist, ptr obj) -- tag-gated; a
    // Callable field may be a bare fn ptr (no header), so it can't go through the
    // raw worklist push (which would atomic-write gc_flags into .text).
    getOrDeclareRuntime("dragon_mark_shared_callable",
        llvm::FunctionType::get(voidType, {i8PtrType, i8PtrType}, false));
    // i64 dragon_class_register_mark_shared(i64 class_id, ptr fn)
    getOrDeclareRuntime("dragon_class_register_mark_shared",
        llvm::FunctionType::get(i64Type, {i64Type, i8PtrType}, false));
    // void dragon_mark_shared_boxed(i64 tag, i64 payload) -- tag dispatched
    // mark for union/nay box stored into a module global (str lef or tag gated
    // clsure / deep or header carrying hepa tags)
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
    //  Allocates header + body. Body layout owned by codegen (per-lambda struct).
    //  gc_fn is the multi-op env GC hook; trackable=1 gc-tracks
    //  the env so a closure-capture cycle through it is collectable.
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
            // Per-module mangling. Extern-C declarations keep the bare name
            // since they reference C-ABI symbols (`malloc`, `dragon_str_*`,
            // etc.) that must NOT be mangled - the linker would not
            // find them. Same-module Dragon defs get mangled so two stdlib
            // modules with `def open` produce distinct LLVM symbols.
            // `extern "C" def CSYM(...) as DRAGON_NAME` stores the
            // C symbol in `externSymbol` and the Dragon-visible alias in
            // `name`. The LLVM symbol must use the C name so the linker
            // resolves it; the Dragon name flows through the alias map
            // populated below.
            const std::string externLinkName =
                func->externSymbol.empty() ? func->name : func->externSymbol;
            const std::string llvmName = func->isExtern
                ? userFuncName(externLinkName)
                : mangleFunc(currentModuleName, func->name);
            if (module->getFunction(llvmName)) {
                // The LLVM symbol already exists (another module declared an
                // extern with the same C symbol - e.g. glob.dr/path.dr both
                // declare `getcwd` - or declareRuntimeFunctions pre-declared a
                // dragon_* symbol). The dedup must NOT skip this module's
                // Dragon-side alias registration: without it, a call
                // to the alias (e.g. os's `_libc_getcwd`) resolves to nothing.
                // It must not skip the RC side maps either: every `dragon_*`
                // extern collides with the runtime pre-declarations, so
                // without this its param kinds and FFI drain eligibility are
                // never registered and owned arg temps at those call sites
                // never drain (stdlib http's nested dragon_str_concat hits
                // exactly this).
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
                        // Derive the element representation from `*args: T`.
                        // The annotation is the per-element type (Python
                        // semantics), so the call site packs into the matching
                        // monomorphized list variant. No annotation -> tag 0
                        // (legacy i64 path, correct for int/bool).
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
                // D030 Phase 4: union params are a single {i64, i64} box - no
                // hidden trailing tag arg. typeExprToLLVM(UnionTypeExpr)
                // returns boxType.
                paramTypes.push_back(typeExprToLLVM(p.type.get()));
                tagMask.push_back(false);
            }
            // Side maps key by the LLVM symbol name (post-mangling), not the
            // bare Dragon name. Two modules with same-named `def open` keep
            // distinct varargs / param kinds / defaults / return-class /
            // generator-flag entries; readers resolve via the same chain
            // (alias -> mangleFunc(currentModule, name) -> userFuncName) via
            // Impl::resolveCalleeSymbol.
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
                // No annotation: a procedure (no value-returning return) is
                // void; otherwise keep the historical int default. Must match
                // the body-emission site (Functions.cpp) or LLVM verify fails.
                retType = unannotatedReturnType(func->body);
            } else {
                retType = typeExprToLLVM(func->returnType.get());
            }
            // D027: order-independent population of funcReturnsClosure. This
            // pre-pass runs before ANY body emission, so a class method that
            // calls a closure factory sees the factory already marked (the
            // visit(FunctionDecl) site alone is too late for method bodies).
            // Keyed by the bare Dragon name to match the call-site lookup.
            if (functionReturnsClosure(*func))
                funcReturnsClosure.insert(func->name);
            // D027: record which params are Callable[...] so the direct-call arg
            // path wraps a bare fn passed there into DragonClosure(fn, null),
            // keeping every Callable value a real DragonClosure (reliable
            // dispatch). Indexed by AST param position.
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
            // GC Phase 4: store param VarKinds for atomic incref at fire/async spawn.
            // D030 Phase 4: union params are a single box arg - one VarKind slot,
            // no tag-arg companion.
            if (options.gcMode == GCMode::RC) {
                std::vector<VarKind> pkinds;
                std::vector<bool> powns;
                for (auto& p : func->params) {
                    pkinds.push_back(typeExprToKind(p.type.get()));
                    powns.push_back(p.isOwn);
                }
                funcParamKinds[llvmName] = std::move(pkinds);
                funcParamOwns[llvmName] = std::move(powns);
                // Extern "C" callees follow the FFI v0 ownership contract (see
                // externDrainableFuncs in CodeGenImpl.h): args borrowed for the
                // call, managed returns fresh; a `ptr` return may alias an
                // argument, so only non-ptr-returning externs are drain-eligible.
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
            // D040: declared parameter names for keyword-argument binding.
            // Vararg/kwarg slots get their declared name (or "" for the bare
            // `*` separator). Call-site kwargs match against these.
            {
                std::vector<std::string> names;
                for (auto& p : func->params)
                    names.push_back(p.name);
                funcParamNames[llvmName] = std::move(names);
            }
            // Track top-level functions whose return type is a class instance
            // so resolveExprClassName can resolve `make_box(...).method()`-style
            // chained dispatch without the receiver being held in a named var.
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
            // Library-detection heuristics inspect the C symbol, not
            // the (possibly aliased) Dragon-visible name. For a plain extern
            // these are the same string; for an aliased extern they diverge.
            // Auto-detect sqlite3 usage for bundled lib linking
            if (func->isExtern && externLinkName.substr(0, 7) == "sqlite3") {
                needsSqlite3 = true;
            }
            // Auto-detect PCRE2 usage for bundled lib linking
            if (func->isExtern && externLinkName.substr(0, 5) == "pcre2") {
                needsPcre2 = true;
            }
            // Auto-detect mbedTLS usage. Two families of runtime entry points
            // (both in dragon_runtime) pull mbedtls_* symbols, so the program
            // must link libdragon_mbedtls.a:
            //  1. the dragon_tls_* TLS shim (runtime_tls.cpp), and
            //  2. the crypto digests / HMAC (runtime_platform.cpp) - dragon_sha*,
            //  dragon_md5*, dragon_hmac - since ADR 038 Phase 7 retired the
            //  hand-rolled cores in favor of mbedTLS.
            if (func->isExtern &&
                (externLinkName.substr(0, 10) == "dragon_tls" ||
                 externLinkName.substr(0, 10) == "dragon_sha" ||
                 externLinkName.substr(0, 10) == "dragon_md5" ||
                 externLinkName.substr(0, 11) == "dragon_hmac")) {
                needsMbedtls = true;
            }
            // Auto-detect zlib / zstd usage for system-lib linking. Same
            // pattern as sqlite3/pcre2: when stdlib gzip.dr / zstandard.dr /
            // tarfile.dr (or any user code) declares an extern reference to
            // dragon_zlib_* / dragon_zstd_*, the runtime archive will pull
            // in compress/decompress symbols that depend on libz/libzstd.
            // Without these flags, programs that don't touch compression
            // skip both -lz and -lzstd at link.
            if (func->isExtern && externLinkName.substr(0, 11) == "dragon_zlib") {
                needsZ = true;
            }
            if (func->isExtern && externLinkName.substr(0, 11) == "dragon_zstd") {
                needsZstd = true;
            }
            // When the extern has an alias, register `name -> llvmName`
            // in the current module's alias scope so a Dragon call to the
            // alias resolves to the C symbol. resolveCalleeSymbol probes this
            // map first.
            if (func->isExtern && !func->externSymbol.empty()) {
                importedFuncAliasesByModule[currentModuleName][func->name] = llvmName;
            }
            (void)llvmFunc;
        }
    }
}

// Forward-declare class constructors and methods in a module.
// Supports multi-constructor overloading: when a class has N>1 __init__
// methods, they are mangled as ClassName___init___0 .. ClassName___init___N-1
// and ClassName_new_0 .. ClassName_new_N-1. When N==1 (or 0), the existing
// un-suffixed names are used so that all legacy code is unaffected.
void CodeGen::Impl::forwardDeclareClasses(dragon::Module& mod) {
    // 6.18: synthesize @dataclass / NamedTuple / enum methods BEFORE scanning
    // for __init__ - the synthesized methods must be visible to the rest of
    // forwardDeclareClasses (which creates the LLVM ctor/method functions).
    // synthesizeEnumMethods runs first: it rewrites `class C(Enum)` into a plain
    // class (members -> singleton statics), so the dataclass pass then sees a
    // normal class and leaves it alone.
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
                typedDictClasses.insert(classDecl->name);
                // Collect field schemas
                for (auto& bs : classDecl->body) {
                    if (auto* ann = dynamic_cast<AnnAssignStmt*>(bs.get())) {
                        if (auto* fn = dynamic_cast<NameExpr*>(ann->target.get())) {
                            typedDictFieldKinds[classDecl->name][fn->name] =
                                typeExprToTypeKind(ann->annotation.get());
                        }
                    }
                }
            } else {
                classNames.insert(classDecl->name);
                // Track which module owns this class so call sites can pick
                // the right `<mod>__<class>_new` symbol when two modules
                // define same-named classes (last-write-wins is acceptable
                // because resolveClassOwningModule prefers the same-module
                // probe over this fallback).
                // D044 cross-module generics: a stamped instantiation lives in
                // the instantiating module's body but is OWNED by the template's
                // defining module, so its symbols match the body emission
                // (Classes.cpp visit(ClassDecl)).
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
            if (typedDictClasses.count(classDecl->name)) continue;

            // D044 cross-module generics: forward-declare a stamped instantiation's
            // struct/ctor/method symbols under the template's DEFINING module so
            // they match the bodies emitted in Classes.cpp visit(ClassDecl) and the
            // call site's resolveClassOwningModule. Restored at iteration end (incl.
            // any early continue below). Inert for normal classes.
            std::string _savedMod = currentModuleName;
            struct RM { std::string* s; std::string v; bool a; ~RM(){ if (a) *s = v; } }
                _rm{&currentModuleName, _savedMod,
                    !classDecl->genericHomeModule.empty()};
            if (!classDecl->genericHomeModule.empty())
                currentModuleName = classDecl->genericHomeModule;

            // Track parent class for MRO. Accept both bare names
            // (`class Foo(Base)`) and dotted module references
            // (`class Foo(unittest.TestCase)`) - the latter is how
            // cross-module inheritance lands in the AST. We store the
            // bare parent name; classOwningModule resolves which module
            // it came from when the descriptor is emitted.
            if (!classDecl->bases.empty()) {
                std::string baseBareName;
                if (auto* baseName = dynamic_cast<NameExpr*>(classDecl->bases[0].get())) {
                    baseBareName = baseName->name;
                } else if (auto* baseAttr =
                               dynamic_cast<AttributeExpr*>(classDecl->bases[0].get())) {
                    baseBareName = baseAttr->attribute;
                }
                if (!baseBareName.empty()) {
                    classParentNames[classDecl->name] = baseBareName;

                    // Detect user-defined exception classes
                    if (isExcType(baseBareName)) {
                        int64_t code = userExcNextCode++;
                        userExcCodes[classDecl->name] = code;
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

            // Per-module class symbol prefix - mirrors mangleFunc routing
            // for top-level functions. Two modules with same-named classes
            // get distinct LLVM symbols so neither body is silently dropped.
            const std::string clsSym = mangleClass(currentModuleName, classDecl->name);

            if (ctorCount == 0) {
                // No explicit constructor - synthesize a default zero-arg one
                // (Python parity: `class Foo: pass` is constructible via the
                // inherited/default __init__). Forward-declare a zero-user-arg
                // `__init__(self)` and `_new()`. The body (Classes.cpp) calls
                // the parent's zero-arg __init__ when present so inherited
                // field initialization still runs. A class that genuinely has
                // no fields and no parent ctor just gets a zero-init instance.
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
                // Zero-arg ctor: register EMPTY param metadata so the entry
                // exists (the ctor call site treats a missing entry as a
                // compiler invariant violation - see the descriptor backstop
                // in CallExpr.cpp).
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

                // Register the ctor's param VarKinds and own flags NOW, before
                // any method body is lowered (fire-own-fwdref-hang.md). The
                // body pass (emitNewBody) re-derives the same data, but that
                // runs in source order - a construction site inside an EARLIER
                // class's method saw no entry for a forward-referenced class,
                // paramIsOwn() answered false, and the call site drained the
                // owned temp an `own` ctor param had adopted (use-after-free
                // on the owned field). Methods already register here (the
                // funcParamOwns loop below); ctors must too.
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
                classCtorCount[classDecl->name] = ctorCount;
                auto& arityVec = classCtorArities[classDecl->name];
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

                    // Same forward registration as the single-ctor path: each
                    // overload's param kinds/own flags must be visible before
                    // any body is lowered (fire-own-fwdref-hang.md).
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
                // D044+ - skip a generic-method TEMPLATE (own type param, T-typed
                // signature); its concrete stamps (empty typeParams) are appended
                // to this same body by the monomorphizer and emitted here.
                if (!methodDecl->typeParams.empty()) continue;

                // Track dunder methods for dispatch. D045: gate on the shared
                // isReservedDunder allowlist so the dispatch table and the
                // predicate are one source of truth (an unrecognized __x__ is
                // already a type-check error, so this never silently drops a
                // method a valid program relies on).
                if (isReservedDunder(methodDecl->name)) {
                    classDunderMethods[classDecl->name].insert(methodDecl->name);
                }

                std::string methodName = clsSym + "_" + methodDecl->name;
                // ADR 010: an overloaded method (class declares >1 with this
                // name) gets a per-index symbol so its monomorphic functions
                // don't collide; the call site appends the same `__ovN`.
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
                // Determine where user params start in the AST param list:
                // - static (.dr or @staticmethod): 0 (no self/cls)
                // - @classmethod: 1 (skip cls param)
                // - implicit self (.dr instance): 0 (self not in params)
                // - explicit self (.py instance): 1 (skip self param)
                size_t mParamStart;
                if (methodDecl->isClassMethod) {
                    mParamStart = 1;  // skip cls
                } else if (methodDecl->isStatic || methodDecl->hasImplicitSelf) {
                    mParamStart = 0;
                } else {
                    mParamStart = 1;  // skip explicit self
                }
                // Build the LLVM param list. A `*args`/`**kwargs` method param
                // collapses to a single i8* (list/dict pointer) exactly like a
                // variadic free function (forwardDeclareFunctions), and its
                // VarArgInfo drives the call-site packing (packVarArgMethodArgs).
                // The bare `*` keyword-only separator (isVarArg with an empty
                // name) has no LLVM param and is skipped everywhere below so the
                // side maps stay aligned to the LLVM parameter count.
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

                // A method whose body contains `yield` is a GENERATOR method:
                // the method function is the WRAPPER and returns the generator
                // object (ptr), exactly like a free-function generator (see
                // visit(FunctionDecl) + buildGeneratorTrampoline). Register it
                // under its mangled symbol so the for-loop generator path
                // recognizes `obj.method(...)` calls.
                // Instance + static generator methods are supported; @classmethod
                // generators are not yet (cls-as-captured-arg is unhandled), so
                // they fall through to the normal path. Keep this condition in
                // lockstep with the method-emission hook in Classes.cpp.
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
                // Register variadic metadata under the (post-mangling, post-ovN)
                // method symbol so the call site packs surplus positionals into
                // the *args list and surplus keywords into the **kwargs dict.
                if (vaInfo.hasVarArg || vaInfo.hasKwArg)
                    funcVarArgInfo[methodName] = vaInfo;
                if (methodIsGenerator) {
                    generatorFunctions.insert(methodName);
                    generatorYieldKinds[methodName] = inferYieldKind(methodDecl->body);
                }

                // D018 fix: store method param VarKinds so the fire-site can
                // emit the correct atomic-incref + mark-shared per arg. Without
                // this, `fire self.method(...)` had funcParamKinds[methodName]
                // empty, so the spawn-site emitted no atomic-incref AND no
                // mark-shared - leaving Router state non-atomic across vthread
                // bodies and triggering the GC-collect crash at request 87.
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
                // D040: parameter names for method keyword-arg binding.
                // Self slot gets "self" (never a valid kwarg name in practice,
                // so it harmlessly never matches; the explicit name keeps the
                // vector length matched to the LLVM param count).
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
                // Track declared return Type::Kind so call sites can pick the
                // right VarKind for a returned ptr-shaped value (str/list/dict
                // etc.). Drives the __next__-binding path in ForLoop.cpp so
                // `for x in iter` carries the right kind through to method
                // dispatch on `x` (e.g. x.strip() for __next__() -> str).
                if (methodDecl->returnType)
                    methodReturnKinds[methodName] =
                        typeExprToTypeKind(methodDecl->returnType.get());

                // 4.1 @property: register getter / setter metadata for this class.
                // Setters are mangled by the parser to "<propName>__setter" so they
                // live in their own vtable slot.
                if (methodDecl->isProperty) {
                    classProperties[classDecl->name].insert(methodDecl->name);
                }
                if (!methodDecl->propertySetterFor.empty()) {
                    classPropertySetters[classDecl->name][methodDecl->propertySetterFor] =
                        methodDecl->name; // already mangled "<prop>__setter"
                }
            }

            // Decision 026: Build vtable method order for this class.
            // Inherit parent's vtable order, then append new methods / record overrides.
            //
            // D033: Also build classOwnMethods (only THIS class's methods, in
            // declaration order) - same loop, just without the parent merge -
            // so dir() / find_method don't have to dedupe inherited methods at
            // runtime (the parent chain walk handles that).
            {
                std::vector<std::string> vtableOrder;
                std::vector<std::string> ownMethods;
                auto parentIt = classParentNames.find(classDecl->name);
                if (parentIt != classParentNames.end()) {
                    auto poIt = classVtableMethodOrder.find(parentIt->second);
                    if (poIt != classVtableMethodOrder.end())
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
                    classMethodKinds[classDecl->name][md->name] = kind;
                    if (md->name == "__init__") continue;
                    ownMethods.push_back(md->name);
                    // Check if already inherited (override -- keep same index)
                    bool found = false;
                    for (auto& existing : vtableOrder) {
                        if (existing == md->name) { found = true; break; }
                    }
                    if (!found) vtableOrder.push_back(md->name);
                }
                classVtableMethodOrder[classDecl->name] = vtableOrder;
                classOwnMethods[classDecl->name] = ownMethods;
                for (size_t i = 0; i < vtableOrder.size(); ++i) {
                    classMethodVtableIndices[classDecl->name][vtableOrder[i]] = (unsigned)i;
                }
            }
        }
    }
}

} // namespace dragon
