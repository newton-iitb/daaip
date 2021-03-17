#ifndef CACHE_SET_DBPV_H
#define CACHE_SET_DBPV_H

#include "cache_set.h"
#include "cache_set_lru.h"


//UInt32 m_glob_core_id;
class CacheSetDBPV : public CacheSet
{
   public:
      CacheSetDBPV(String cfgname, core_id_t core_id,
            CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize, CacheSetInfoLRU* set_info, UInt8 num_attempts);
      ~CacheSetDBPV();

      UInt32 getReplacementIndex(CacheCntlr *cntlr,core_id_t core_id);
      void updateReplacementIndex(UInt32 accessed_index);


   private:
      const UInt8  m_rrip_numbits;
      const UInt8  m_rrip_max;
      const UInt8  m_rrip_insert;
      const UInt8  m_num_attempts;
            UInt8 *m_rrip_bits;
            UInt8 *m_block_owner;
            UInt8 *m_block_access; /* Number of times block got accessed */
            UInt8  m_replacement_pointer;
            UInt8  m_case;
      CacheSetInfoLRU* m_set_info;
};

#endif /* CACHE_SET_H */
