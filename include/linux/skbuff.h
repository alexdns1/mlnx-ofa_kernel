#ifndef _COMPAT_LINUX_SKBUFF_H
#define _COMPAT_LINUX_SKBUFF_H

#include "../../compat/config.h"
#include <linux/version.h>

#include_next <linux/skbuff.h>

#ifndef SKB_TRUESIZE
#define SKB_TRUESIZE(X) ((X) +						\
			SKB_DATA_ALIGN(sizeof(struct sk_buff)) +	\
			SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))
#endif

#ifndef HAVE_SKB_IS_GSO_TCP
/* Note: Should be called only if skb_is_gso(skb) is true */
static inline bool skb_is_gso_tcp(const struct sk_buff *skb)
{
	return skb_shinfo(skb)->gso_type & (SKB_GSO_TCPV4 | SKB_GSO_TCPV6);
}
#endif

#ifndef HAVE_SKB_FRAG_OFF_ADD
static inline void skb_frag_off_add(skb_frag_t *frag, int delta)
{
	frag->page_offset += delta;
}
#endif

#ifndef HAVE_SKB_FRAG_OFF_SET
static inline void skb_frag_off_set(skb_frag_t *frag, unsigned int offset)
{
	frag->page_offset = offset;
}
#endif
#endif /* _COMPAT_LINUX_SKBUFF_H */
