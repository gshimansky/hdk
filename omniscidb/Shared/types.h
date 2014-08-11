/* 
 * File:        types.h
 * Author(s):   steve@map-d.com
 *
 * Created on June 19, 2014, 4:29 PM
 */

#ifndef _TYPES_H
#define	_TYPES_H

#include <vector>
#include <cstddef>

// definition of a byte type
typedef unsigned char mapd_byte_t;

// definition of a memory address type
typedef mapd_byte_t* mapd_addr_t;

// definition of Map-D size type
typedef std::size_t mapd_size_t;

// The ChunkKey is a unique identifier for chunks in the database file.
// The first element of the underlying vector for ChunkKey indicates the type of
// ChunkKey (also referred to as the keyspace id)
typedef std::vector<int> ChunkKey;

/**
 * DerefSort is used for sorting pointers to comparable types/objects when they
 * are stored in a sorted STL container such as a set. It simply dereferences the
 * pointers so that the sort predicate implemented for the object is used, instead
 * of a pointer comparison.
 */
template <typename T>
struct DerefSort {
	bool operator() (const T* lhs, const T* rhs) const {
		return (*lhs < *rhs);
	}
};

enum mapd_data_t {
    INT_TYPE,
    FLOAT_TYPE,
    BOOLEAN_TYPE
};
//@todo should this be somewhere else
mapd_size_t getBitSizeForType(const mapd_data_t dataType) {
    switch (dataType) {
        case INT_TYPE:
        case FLOAT_TYPE:
            return 32;
            break;
        case BOOLEAN_TYPE:
            return 1;
            break;
    }
}

#endif	/* _TYPES_H */

