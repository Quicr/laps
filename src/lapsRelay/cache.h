
#include <list>
#include <set>
#include <map>
#include <vector>

#include <laps.h>


class Cache {
public:
  Cache() {};

  void put(const MsgShortName& name, const std::vector<uint8_t>& data );

  const std::vector<uint8_t>* get( const MsgShortName& name ) const;

  bool exists(  const MsgShortName& name ) const;

  std::list<MsgShortName> find(const MsgShortName& name, const int len ) const;

  ~Cache();
  
private:
  std::map< MsgShortName, std::vector<uint8_t>* > dataCache;
  const std::vector<uint8_t> emptyVec;
};
