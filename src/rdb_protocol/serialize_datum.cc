// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/serialize_datum.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/error.hpp"

namespace ql {


enum class datum_serialized_type_t {
    R_ARRAY = 1,
    R_BOOL = 2,
    R_NULL = 3,
    DOUBLE = 4,
    R_OBJECT = 5,
    R_STR = 6,
    INT_NEGATIVE = 7,
    INT_POSITIVE = 8,
    R_BINARY = 9,
    LAZY = 10
};
// TODO! Make sure we update block magics

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(datum_serialized_type_t, int8_t,
                                      datum_serialized_type_t::R_ARRAY,
                                      datum_serialized_type_t::LAZY);

serialization_result_t datum_serialize(write_message_t *wm,
                                       datum_serialized_type_t type) {
    serialize<cluster_version_t::LATEST_OVERALL>(wm, type);
    return serialization_result_t::SUCCESS;
}

MUST_USE archive_result_t datum_deserialize(read_stream_t *s,
                                            datum_serialized_type_t *type) {
    return deserialize<cluster_version_t::LATEST_OVERALL>(s, type);
}

// This looks like it duplicates code of other deserialization functions.  It does.
// Keeping this separate means that we don't have to worry about whether datum
// serialization has changed from cluster version to cluster version.

// Keep in sync with datum_serialize.
size_t datum_serialized_size(const std::vector<counted_t<const datum_t> > &v) {
    size_t ret = varint_uint64_serialized_size(v.size());
    for (auto it = v.begin(), e = v.end(); it != e; ++it) {
        ret += datum_serialized_size(*it);
    }
    return ret;
}


// Keep in sync with datum_serialized_size.
serialization_result_t datum_serialize(write_message_t *wm,
                                       const std::vector<counted_t<const datum_t> > &v) {
    serialization_result_t res = serialization_result_t::SUCCESS;
    serialize_varint_uint64(wm, v.size());
    for (auto it = v.begin(), e = v.end(); it != e; ++it) {
        res = res | datum_serialize(wm, *it);
    }
    return res;
}

MUST_USE archive_result_t
datum_deserialize(read_stream_t *s, std::vector<counted_t<const datum_t> > *v) {
    v->clear();

    uint64_t sz;
    archive_result_t res = deserialize_varint_uint64(s, &sz);
    if (bad(res)) { return res; }

    if (sz > std::numeric_limits<size_t>::max()) {
        return archive_result_t::RANGE_ERROR;
    }

    v->resize(sz);
    for (uint64_t i = 0; i < sz; ++i) {
        res = datum_deserialize(s, &(*v)[i]);
        if (bad(res)) { return res; }
    }

    return archive_result_t::SUCCESS;
}

size_t datum_serialized_size(const std::string &s) {
    return serialize_universal_size(s);
}
serialization_result_t datum_serialize(write_message_t *wm, const std::string &s) {
    serialize_universal(wm, s);
    return serialization_result_t::SUCCESS;
}
MUST_USE archive_result_t datum_deserialize(read_stream_t *s, std::string *out) {
    return deserialize_universal(s, out);
}


size_t datum_serialized_size(
        const std::map<std::string, counted_t<const datum_t> > &m) {
    size_t ret = varint_uint64_serialized_size(m.size());
    for (auto it = m.begin(), e = m.end(); it != e; ++it) {
        ret += datum_serialized_size(it->first);
        ret += datum_serialized_size(it->second);
    }
    return ret;
}

serialization_result_t
datum_serialize(write_message_t *wm,
                const std::map<std::string, counted_t<const datum_t> > &m) {
    serialization_result_t res = serialization_result_t::SUCCESS;
    serialize_varint_uint64(wm, m.size());
    for (auto it = m.begin(), e = m.end(); it != e; ++it) {
        res = res | datum_serialize(wm, it->first);
        res = res | datum_serialize(wm, it->second);
    }
    return res;
}

MUST_USE archive_result_t datum_deserialize(
        read_stream_t *s,
        std::map<std::string, counted_t<const datum_t> > *m) {
    m->clear();

    uint64_t sz;
    archive_result_t res = deserialize_varint_uint64(s, &sz);
    if (bad(res)) { return res; }

    if (sz > std::numeric_limits<size_t>::max()) {
        return archive_result_t::RANGE_ERROR;
    }

    // Using position should make this function take linear time, not
    // sz*log(sz) time.
    auto position = m->begin();

    for (uint64_t i = 0; i < sz; ++i) {
        std::pair<std::string, counted_t<const datum_t> > p;
        res = datum_deserialize(s, &p.first);
        if (bad(res)) { return res; }
        res = datum_deserialize(s, &p.second);
        if (bad(res)) { return res; }
        position = m->insert(position, std::move(p));
    }

    return archive_result_t::SUCCESS;
}


size_t datum_lazy_serialized_size(const counted_t<const datum_t> &datum) {
    r_sanity_check(datum.has());

    if (datum->is_lazy()) {
        return datum->as_lazy_serialized().size();
    }

    size_t sz = 1; // 1 byte for the type

    switch (datum->get_type()) {
    case datum_t::R_ARRAY: {
        sz += datum_serialized_size(datum->as_array());
    } break;
    case datum_t::R_BINARY: {
        sz += datum_serialized_size(datum->as_binary());
    } break;
    case datum_t::R_BOOL: {
        sz += serialize_universal_size_t<bool>::value;
    } break;
    case datum_t::R_NULL: break;
    case datum_t::R_NUM: {
        double d = datum->as_num();
        int64_t i;
        if (number_as_integer(d, &i)) {
            sz += varint_uint64_serialized_size(i < 0 ? -i : i);
        } else {
            sz += serialize_universal_size_t<double>::value;
        }
    } break;
    case datum_t::R_OBJECT: {
        sz += datum_serialized_size(datum->as_object());
    } break;
    case datum_t::R_STR: {
        sz += datum_serialized_size(datum->as_str());
    } break;
    case datum_t::LAZY_SERIALIZED: // fallthru
    default:
        unreachable();
    }

    return sz;
}

size_t datum_serialized_size(const counted_t<const datum_t> &datum) {
    // TODO! De-dup
    bool serialize_as_lazy = datum->is_lazy();
    if (!serialize_as_lazy) {
        // Note that we must only call get_type() once we have determined
        // that the datum is not currently lazy. Otherwise get_type() will
        // destroy the datum's current lazyness.
        datum_t::type_t type = datum->get_type();
        if (type == datum_t::R_ARRAY || type == datum_t::R_OBJECT) {
            serialize_as_lazy = true;
        }
    }

    size_t sz = 0;
    if (serialize_as_lazy) {
        sz += 1; // 1 byte for the lazy type
        sz += sizeof(uint64_t); // 8 bytes for the serialized size
    }

    sz += datum_lazy_serialized_size(datum);
    return sz;
}
serialization_result_t datum_serialize(write_message_t *wm,
                                       const counted_t<const datum_t> &datum) {
    serialization_result_t res = serialization_result_t::SUCCESS;
    r_sanity_check(datum.has());

    // Determine whether it makes sense for deserialization of this to be lazy
    bool serialize_as_lazy = datum->is_lazy();
    if (!serialize_as_lazy) {
        // Note that we must only call get_type() once we have determined
        // that the datum is not currently lazy. Otherwise get_type() will
        // destroy the datum's current lazyness.
        datum_t::type_t type = datum->get_type();
        if (type == datum_t::R_ARRAY || type == datum_t::R_OBJECT) {
            serialize_as_lazy = true;
        }
    }

    if (serialize_as_lazy) {
        res = res | datum_serialize(wm, datum_serialized_type_t::LAZY);
        uint64_t serialized_size = datum_lazy_serialized_size(datum);
        serialize_universal(wm, serialized_size);
    }

    if (datum->is_lazy()) {
        rassert(serialize_as_lazy);
        wm->append(datum->as_lazy_serialized().data(),
                   datum->as_lazy_serialized().size());
        return res;
    }

    switch (datum->get_type()) {
    case datum_t::R_ARRAY: {
        res = res | datum_serialize(wm, datum_serialized_type_t::R_ARRAY);
        const std::vector<counted_t<const datum_t> > &value = datum->as_array();
        if (value.size() > 100000)
            res = res | serialization_result_t::ARRAY_TOO_BIG;
        res = res | datum_serialize(wm, value);
    } break;
    case datum_t::R_BINARY: {
        datum_serialize(wm, datum_serialized_type_t::R_BINARY);
        const wire_string_t &value = datum->as_binary();
        datum_serialize(wm, value);
    } break;
    case datum_t::R_BOOL: {
        res = res | datum_serialize(wm, datum_serialized_type_t::R_BOOL);
        bool value = datum->as_bool();
        serialize_universal(wm, value);
    } break;
    case datum_t::R_NULL: {
        res = res | datum_serialize(wm, datum_serialized_type_t::R_NULL);
    } break;
    case datum_t::R_NUM: {
        double value = datum->as_num();
        int64_t i;
        if (number_as_integer(value, &i)) {
            // We serialize the signed-zero double, -0.0, with INT_NEGATIVE.

            // so we can use `signbit` in a GCC 4.4.3-compatible way
            using namespace std;  // NOLINT(build/namespaces)
            if (signbit(value)) {
                res = res | datum_serialize(wm, datum_serialized_type_t::INT_NEGATIVE);
                serialize_varint_uint64(wm, -i);
            } else {
                res = res | datum_serialize(wm, datum_serialized_type_t::INT_POSITIVE);
                serialize_varint_uint64(wm, i);
            }
        } else {
            res = res | datum_serialize(wm, datum_serialized_type_t::DOUBLE);
            serialize_universal(wm, value);
        }
    } break;
    case datum_t::R_OBJECT: {
        res = res | datum_serialize(wm, datum_serialized_type_t::R_OBJECT);
        res = res | datum_serialize(wm, datum->as_object());
    } break;
    case datum_t::R_STR: {
        res = res | datum_serialize(wm, datum_serialized_type_t::R_STR);
        const wire_string_t &value = datum->as_str();
        res = res | datum_serialize(wm, value);
    } break;
    case datum_t::LAZY_SERIALIZED: // fallthru
    default:
        unreachable();
    }
    return res;
}
archive_result_t datum_deserialize(read_stream_t *s, counted_t<const datum_t> *datum) {
    datum_serialized_type_t type;
    archive_result_t res = datum_deserialize(s, &type);
    if (bad(res)) {
        return res;
    }

    if (type == datum_serialized_type_t::LAZY) {
        // This just loads the datum as a LAZY_SERIALIZED datum. The actual
        // deserialization happens when the datum is first accessed.
        uint64_t serialized_size;
        res = deserialize_universal(s, &serialized_size);
        if (bad(res)) {
            return res;
        }
        if (serialized_size > std::numeric_limits<size_t>::max()) {
            return archive_result_t::RANGE_ERROR;
        }

        std::vector<char> serialized_datum(static_cast<size_t>(serialized_size));
        int64_t num_read = force_read(s, serialized_datum.data(), serialized_size);
        if (num_read == -1) {
            return archive_result_t::SOCK_ERROR;
        }
        if (static_cast<uint64_t>(num_read) < serialized_size) {
            return archive_result_t::SOCK_EOF;
        }
        datum->reset(new datum_t(std::move(serialized_datum)));
        return archive_result_t::SUCCESS;;
    } else {
        // Deserialize the datum immediately
        // TODO! Refactor maybe?
        *datum = datum_t::null();
        // TODO! Again, the interface for this sucks
        return deserialize_lazy_datum(type, s, const_cast<datum_t::data_wrapper_t*>(&(*datum)->data));
    }
}

// TODO! This is a terrible interface
archive_result_t deserialize_lazy_datum(read_stream_t *s, datum_t::data_wrapper_t *data) {
    datum_serialized_type_t type;
    archive_result_t res = datum_deserialize(s, &type);
    if (bad(res)) {
        return res;
    }
    return deserialize_lazy_datum(type, s, data);
}
archive_result_t deserialize_lazy_datum(datum_serialized_type_t type, read_stream_t *s, datum_t::data_wrapper_t *data) {
    archive_result_t res = archive_result_t::SUCCESS;
    switch (type) {
    case datum_serialized_type_t::R_ARRAY: {
        std::vector<counted_t<const datum_t> > value;
        res = datum_deserialize(s, &value);
        if (bad(res)) {
            return res;
        }
        data->type = datum_t::R_ARRAY;
        data->r_array = new std::vector<counted_t<const datum_t> >(std::move(value));
    } break;
    case datum_serialized_type_t::R_BINARY: {
        scoped_ptr_t<wire_string_t> value;
        res = datum_deserialize(s, &value);
        if (bad(res)) {
            return res;
        }
        rassert(value.has());
        data->type = datum_t::R_BINARY;
        data->r_str = value.release();
    } break;
    case datum_serialized_type_t::R_BOOL: {
        bool value;
        res = deserialize_universal(s, &value);
        if (bad(res)) {
            return res;
        }
        data->type = datum_t::R_BOOL;
        data->r_bool = value;
    } break;
    case datum_serialized_type_t::R_NULL: {
        data->type = datum_t::R_NULL;
    } break;
    case datum_serialized_type_t::DOUBLE: {
        double value;
        res = deserialize_universal(s, &value);
        if (bad(res)) {
            return res;
        }
        data->type = datum_t::R_NUM;
        data->r_num = value;
    } break;
    case datum_serialized_type_t::INT_NEGATIVE:  // fall through
    case datum_serialized_type_t::INT_POSITIVE: {
        uint64_t unsigned_value;
        res = deserialize_varint_uint64(s, &unsigned_value);
        if (bad(res)) {
            return res;
        }
        if (unsigned_value > max_dbl_int) {
            return archive_result_t::RANGE_ERROR;
        }
        const double d = unsigned_value;
        double value;
        if (type == datum_serialized_type_t::INT_NEGATIVE) {
            // This might deserialize the signed-zero double, -0.0.
            value = -d;
        } else {
            value = d;
        }
        data->type = datum_t::R_NUM;
        data->r_num = value;
    } break;
    case datum_serialized_type_t::R_OBJECT: {
        std::map<std::string, counted_t<const datum_t> > value;
        res = datum_deserialize(s, &value);
        if (bad(res)) {
            return res;
        }
        data->type = datum_t::R_OBJECT;
        data->r_object = new std::map<std::string, counted_t<const datum_t> >(std::move(value));
    } break;
    case datum_serialized_type_t::R_STR: {
        scoped_ptr_t<wire_string_t> value;
        res = datum_deserialize(s, &value);
        if (bad(res)) {
            return res;
        }
        rassert(value.has());
        data->type = datum_t::R_STR;
        data->r_str = value.release();
    } break;
    case datum_serialized_type_t::LAZY: // fallthru
    default:
        return archive_result_t::RANGE_ERROR;
    }

    return res;
}


size_t datum_serialized_size(const wire_string_t &s) {
    return varint_uint64_serialized_size(s.size()) + s.size();
}

serialization_result_t datum_serialize(write_message_t *wm, const wire_string_t &s) {
    serialize_varint_uint64(wm, static_cast<uint64_t>(s.size()));
    wm->append(s.data(), s.size());
    return serialization_result_t::SUCCESS;
}

MUST_USE archive_result_t datum_deserialize(
        read_stream_t *s,
        scoped_ptr_t<wire_string_t> *out) {
    uint64_t sz;
    archive_result_t res = deserialize_varint_uint64(s, &sz);
    if (res != archive_result_t::SUCCESS) { return res; }

    if (sz > std::numeric_limits<size_t>::max()) {
        return archive_result_t::RANGE_ERROR;
    }

    scoped_ptr_t<wire_string_t> value(wire_string_t::create(sz));

    int64_t num_read = force_read(s, value->data(), sz);
    if (num_read == -1) {
        return archive_result_t::SOCK_ERROR;
    }
    if (static_cast<uint64_t>(num_read) < sz) {
        return archive_result_t::SOCK_EOF;
    }

    *out = std::move(value);

    return archive_result_t::SUCCESS;
}


template <cluster_version_t W>
void serialize(write_message_t *wm,
               const empty_ok_t<const counted_t<const datum_t> > &datum) {
    const counted_t<const datum_t> *pointer = datum.get();
    const bool has = pointer->has();
    serialize<W>(wm, has);
    if (has) {
        serialize<W>(wm, *pointer);
    }
}

INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(empty_ok_t<const counted_t<const datum_t> >);

template <cluster_version_t W>
archive_result_t deserialize(read_stream_t *s,
                             empty_ok_ref_t<counted_t<const datum_t> > datum) {
    bool has;
    archive_result_t res = deserialize<W>(s, &has);
    if (bad(res)) {
        return res;
    }

    counted_t<const datum_t> *pointer = datum.get();

    if (!has) {
        pointer->reset();
        return archive_result_t::SUCCESS;
    } else {
        return deserialize<W>(s, pointer);
    }
}

template archive_result_t deserialize<cluster_version_t::v1_13>(
        read_stream_t *s,
        empty_ok_ref_t<counted_t<const datum_t> > datum);
template archive_result_t deserialize<cluster_version_t::v1_13_2>(
        read_stream_t *s,
        empty_ok_ref_t<counted_t<const datum_t> > datum);
template archive_result_t deserialize<cluster_version_t::v1_14_is_latest>(
        read_stream_t *s,
        empty_ok_ref_t<counted_t<const datum_t> > datum);


}  // namespace ql
