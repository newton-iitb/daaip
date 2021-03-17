#ifndef CACHE_SET_UCP_SRRIP_ATD_H
#define CACHE_SET_UCP_SRRIP_ATD_H

#include "cache_set.h"
#include "cache_set_lru.h"

class CacheSetUCP_SRRIP_ATD : public CacheSet
{
   public:
      CacheSetUCP_SRRIP_ATD(String cfgname, core_id_t core_id,
            CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize, CacheSetInfoLRU* set_info, UInt8 num_attempts);
      ~CacheSetUCP_SRRIP_ATD();

      UInt32 getReplacementIndex(CacheCntlr *cntlr, core_id_t core_id);
      void updateReplacementIndex(UInt32 accessed_index);


   private:
        UInt8 *m_rrip_bits;
        UInt8 *m_block_owner;
        UInt8  m_replacement_pointer;
        UInt32 m_setID;
};

#endif /* CACHE_SET_UCP_SRRIP_ATD_H */
