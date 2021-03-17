#ifndef CACHE_SET_DBASP_H
#define CACHE_SET_DBASP_H

#include "cache_set.h"
#include "cache_set_lru.h"


class CacheSetDBASP : public CacheSet
{
   public:
      CacheSetDBASP(String cfgname, core_id_t core_id,
            CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize, CacheSetInfoLRU* set_info, UInt8 num_attempts);
      ~CacheSetDBASP();

      UInt32 InsertBlockAtIndex(UInt32 index, core_id_t core_id);
      UInt32 getReplacementIndex(CacheCntlr *cntlr, core_id_t core_id);
      void updateReplacementIndex(UInt32 accessed_index);


   private:
      const UInt32 m_saturation_counter_max_value;
      const UInt32 m_db_percent_threshold;
      const UInt8  m_rrip_numbits;
      const UInt8  m_rrip_max;
      const UInt8  m_rrip_insert;
            UInt8 *m_rrip_bits;
            UInt8 *m_block_owner;
            UInt8 *m_block_access; /* Number of times block got accessed */
            UInt8  m_replacement_pointer;
            UInt32 m_setID;
      CacheSetInfoLRU* m_set_info;
};

#endif /* CACHE_SET_DBASP_H */
