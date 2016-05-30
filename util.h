#ifndef DUMMY_TFTPD_UTIL_H
#define DUMMY_TFTPD_UTIL_H
#include <stdint.h>

#define list_append(head_, elt_) do {				\
	__typeof__((head_)) head = (head_);			\
	__typeof__((elt_)) elt = (elt_);			\
	if (!*head) {						\
		*head = elt;					\
		break;						\
	}							\
	for (__typeof__(elt) c = *head; c; c = c->next) {	\
		if (!c->next) {					\
			c->next = elt;				\
			break;					\
		}						\
	}							\
} while (0)

#define list_remove(head_, elt_) do {				\
	__typeof__((head_)) head = (head_);			\
	__typeof__((elt_)) elt = (elt_);			\
	if (!*head || !elt) {					\
		break;						\
	}							\
	if (elt == *head) {					\
		*head = elt->next;				\
		elt->next = NULL;				\
		break;						\
	}							\
	for (__typeof__(elt) c = *head; c; c = c->next) {	\
		if (c->next == elt) {				\
			c->next = elt->next;			\
			elt->next = NULL;				\
			break;					\
		}						\
	}							\
} while (0)

#define array_elt_swap(base, i, j) do {					\
	__typeof__(i) i__ = (i);					\
	__typeof__(i) j__ = (j);					\
	__typeof__(base) base__ = (base);				\
	__typeof__(*base__) tmp = base__[i__];				\
	base__[i__] = base__[j__];					\
	base__[j__] = tmp;						\
} while (0)

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int:-!!(e)*1234; }))
#define SAME_TYPES(t1, t2) (__builtin_types_compatible_p(__typeof__(t1), __typeof__(t2)))
#define MUST_BE_ARRAY(a) BUILD_BUG_ON_ZERO(SAME_TYPES((a), &(a)[0]))
#define ARRAY_SIZE(arr) ((sizeof(arr)/sizeof(arr[0])) + MUST_BE_ARRAY(arr))


static inline uint64_t pack_fd_as_ptr(int fd) {
	uint64_t asptr = (uint64_t)fd;
	return (asptr << 1) | 1U;
}

static inline int is_ptr_fd(uint64_t pval) {
	return pval & 1U;
}

static inline int unpack_fd_from_ptr(uint64_t pval)
{
	return (int)(pval >> 1);
}

#endif /* DUMMY_TFTPD_UTIL_H */
