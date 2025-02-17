/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/Variant.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

struct ClampedU8 {
};

// 25.1.1 Notation (read-modify-write modification function), https://tc39.es/ecma262/#sec-arraybuffer-notation
using ReadWriteModifyFunction = Function<ByteBuffer(ByteBuffer, ByteBuffer)>;

class ArrayBuffer : public Object {
    JS_OBJECT(ArrayBuffer, Object);

public:
    static ThrowCompletionOr<NonnullGCPtr<ArrayBuffer>> create(Realm&, size_t);
    static NonnullGCPtr<ArrayBuffer> create(Realm&, ByteBuffer);
    static NonnullGCPtr<ArrayBuffer> create(Realm&, ByteBuffer*);

    virtual ~ArrayBuffer() override = default;

    size_t byte_length() const { return buffer_impl().size(); }
    ByteBuffer& buffer() { return buffer_impl(); }
    ByteBuffer const& buffer() const { return buffer_impl(); }

    // Used by allocate_array_buffer() to attach the data block after construction
    void set_buffer(ByteBuffer buffer) { m_buffer = move(buffer); }

    Value detach_key() const { return m_detach_key; }
    void set_detach_key(Value detach_key) { m_detach_key = detach_key; }

    void detach_buffer() { m_buffer = Empty {}; }
    bool is_detached() const { return m_buffer.has<Empty>(); }

    enum Order {
        SeqCst,
        Unordered
    };
    template<typename type>
    Value get_value(size_t byte_index, bool is_typed_array, Order, bool is_little_endian = true);
    template<typename type>
    void set_value(size_t byte_index, Value value, bool is_typed_array, Order, bool is_little_endian = true);
    template<typename T>
    Value get_modify_set_value(size_t byte_index, Value value, ReadWriteModifyFunction operation, bool is_little_endian = true);

private:
    ArrayBuffer(ByteBuffer buffer, Object& prototype);
    ArrayBuffer(ByteBuffer* buffer, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    ByteBuffer& buffer_impl()
    {
        ByteBuffer* ptr { nullptr };
        m_buffer.visit([&](Empty) { VERIFY_NOT_REACHED(); }, [&](auto* pointer) { ptr = pointer; }, [&](auto& value) { ptr = &value; });
        return *ptr;
    }

    ByteBuffer const& buffer_impl() const { return const_cast<ArrayBuffer*>(this)->buffer_impl(); }

    Variant<Empty, ByteBuffer, ByteBuffer*> m_buffer;
    // The various detach related members of ArrayBuffer are not used by any ECMA262 functionality,
    // but are required to be available for the use of various harnesses like the Test262 test runner.
    Value m_detach_key;
};

void copy_data_block_bytes(ByteBuffer& to_block, u64 to_index, ByteBuffer& from_block, u64 from_index, u64 count);
ThrowCompletionOr<ArrayBuffer*> allocate_array_buffer(VM&, FunctionObject& constructor, size_t byte_length);
ThrowCompletionOr<void> detach_array_buffer(VM&, ArrayBuffer& array_buffer, Optional<Value> key = {});
ThrowCompletionOr<ArrayBuffer*> clone_array_buffer(VM&, ArrayBuffer& source_buffer, size_t source_byte_offset, size_t source_length);

// 25.1.2.9 RawBytesToNumeric ( type, rawBytes, isLittleEndian ), https://tc39.es/ecma262/#sec-rawbytestonumeric
template<typename T>
static Value raw_bytes_to_numeric(VM& vm, ByteBuffer raw_value, bool is_little_endian)
{
    if (!is_little_endian) {
        VERIFY(raw_value.size() % 2 == 0);
        for (size_t i = 0; i < raw_value.size() / 2; ++i)
            swap(raw_value[i], raw_value[raw_value.size() - 1 - i]);
    }
    using UnderlyingBufferDataType = Conditional<IsSame<ClampedU8, T>, u8, T>;
    if constexpr (IsSame<UnderlyingBufferDataType, float>) {
        float value;
        raw_value.span().copy_to({ &value, sizeof(float) });
        if (isnan(value))
            return js_nan();
        return Value(value);
    }
    if constexpr (IsSame<UnderlyingBufferDataType, double>) {
        double value;
        raw_value.span().copy_to({ &value, sizeof(double) });
        if (isnan(value))
            return js_nan();
        return Value(value);
    }
    if constexpr (!IsIntegral<UnderlyingBufferDataType>)
        VERIFY_NOT_REACHED();
    UnderlyingBufferDataType int_value = 0;
    raw_value.span().copy_to({ &int_value, sizeof(UnderlyingBufferDataType) });
    if constexpr (sizeof(UnderlyingBufferDataType) == 8) {
        if constexpr (IsSigned<UnderlyingBufferDataType>) {
            static_assert(IsSame<UnderlyingBufferDataType, i64>);
            return BigInt::create(vm, Crypto::SignedBigInteger { int_value });
        } else {
            static_assert(IsOneOf<UnderlyingBufferDataType, u64, double>);
            return BigInt::create(vm, Crypto::SignedBigInteger { Crypto::UnsignedBigInteger { int_value } });
        }
    } else {
        return Value(int_value);
    }
}

// Implementation for 25.1.2.10 GetValueFromBuffer, used in TypedArray<T>::get_value_from_buffer().
template<typename T>
Value ArrayBuffer::get_value(size_t byte_index, [[maybe_unused]] bool is_typed_array, Order, bool is_little_endian)
{
    auto& vm = this->vm();

    auto element_size = sizeof(T);

    // FIXME: Check for shared buffer

    // FIXME: Propagate errors.
    auto raw_value = MUST(buffer_impl().slice(byte_index, element_size));
    return raw_bytes_to_numeric<T>(vm, move(raw_value), is_little_endian);
}

// 25.1.2.11 NumericToRawBytes ( type, value, isLittleEndian ), https://tc39.es/ecma262/#sec-numerictorawbytes
template<typename T>
static ByteBuffer numeric_to_raw_bytes(VM& vm, Value value, bool is_little_endian)
{
    VERIFY(value.is_number() || value.is_bigint());
    using UnderlyingBufferDataType = Conditional<IsSame<ClampedU8, T>, u8, T>;
    ByteBuffer raw_bytes = ByteBuffer::create_uninitialized(sizeof(UnderlyingBufferDataType)).release_value_but_fixme_should_propagate_errors(); // FIXME: Handle possible OOM situation.
    auto flip_if_needed = [&]() {
        if (is_little_endian)
            return;
        VERIFY(sizeof(UnderlyingBufferDataType) % 2 == 0);
        for (size_t i = 0; i < sizeof(UnderlyingBufferDataType) / 2; ++i)
            swap(raw_bytes[i], raw_bytes[sizeof(UnderlyingBufferDataType) - 1 - i]);
    };
    if constexpr (IsSame<UnderlyingBufferDataType, float>) {
        float raw_value = MUST(value.to_double(vm));
        ReadonlyBytes { &raw_value, sizeof(float) }.copy_to(raw_bytes);
        flip_if_needed();
        return raw_bytes;
    }
    if constexpr (IsSame<UnderlyingBufferDataType, double>) {
        double raw_value = MUST(value.to_double(vm));
        ReadonlyBytes { &raw_value, sizeof(double) }.copy_to(raw_bytes);
        flip_if_needed();
        return raw_bytes;
    }
    if constexpr (!IsIntegral<UnderlyingBufferDataType>)
        VERIFY_NOT_REACHED();
    if constexpr (sizeof(UnderlyingBufferDataType) == 8) {
        UnderlyingBufferDataType int_value;

        if constexpr (IsSigned<UnderlyingBufferDataType>)
            int_value = MUST(value.to_bigint_int64(vm));
        else
            int_value = MUST(value.to_bigint_uint64(vm));

        ReadonlyBytes { &int_value, sizeof(UnderlyingBufferDataType) }.copy_to(raw_bytes);
        flip_if_needed();
        return raw_bytes;
    } else {
        UnderlyingBufferDataType int_value;
        if constexpr (IsSigned<UnderlyingBufferDataType>) {
            if constexpr (sizeof(UnderlyingBufferDataType) == 4)
                int_value = MUST(value.to_i32(vm));
            else if constexpr (sizeof(UnderlyingBufferDataType) == 2)
                int_value = MUST(value.to_i16(vm));
            else
                int_value = MUST(value.to_i8(vm));
        } else {
            if constexpr (sizeof(UnderlyingBufferDataType) == 4)
                int_value = MUST(value.to_u32(vm));
            else if constexpr (sizeof(UnderlyingBufferDataType) == 2)
                int_value = MUST(value.to_u16(vm));
            else if constexpr (!IsSame<T, ClampedU8>)
                int_value = MUST(value.to_u8(vm));
            else
                int_value = MUST(value.to_u8_clamp(vm));
        }
        ReadonlyBytes { &int_value, sizeof(UnderlyingBufferDataType) }.copy_to(raw_bytes);
        if constexpr (sizeof(UnderlyingBufferDataType) % 2 == 0)
            flip_if_needed();
        return raw_bytes;
    }
}

// 25.1.2.12 SetValueInBuffer ( arrayBuffer, byteIndex, type, value, isTypedArray, order [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-setvalueinbuffer
template<typename T>
void ArrayBuffer::set_value(size_t byte_index, Value value, [[maybe_unused]] bool is_typed_array, Order, bool is_little_endian)
{
    auto& vm = this->vm();

    auto raw_bytes = numeric_to_raw_bytes<T>(vm, value, is_little_endian);

    // FIXME: Check for shared buffer

    raw_bytes.span().copy_to(buffer_impl().span().slice(byte_index));
}

// 25.1.2.13 GetModifySetValueInBuffer ( arrayBuffer, byteIndex, type, value, op [ , isLittleEndian ] ), https://tc39.es/ecma262/#sec-getmodifysetvalueinbuffer
template<typename T>
Value ArrayBuffer::get_modify_set_value(size_t byte_index, Value value, ReadWriteModifyFunction operation, bool is_little_endian)
{
    auto& vm = this->vm();

    auto raw_bytes = numeric_to_raw_bytes<T>(vm, value, is_little_endian);

    // FIXME: Check for shared buffer

    // FIXME: Propagate errors.
    auto raw_bytes_read = MUST(buffer_impl().slice(byte_index, sizeof(T)));
    auto raw_bytes_modified = operation(raw_bytes_read, raw_bytes);
    raw_bytes_modified.span().copy_to(buffer_impl().span().slice(byte_index));

    return raw_bytes_to_numeric<T>(vm, raw_bytes_read, is_little_endian);
}

}
