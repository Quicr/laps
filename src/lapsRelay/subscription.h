

#include <list>
#include <set>
#include <map>
#include <vector>

#include <laps.h>


class Subscriptions {
public:

  Subscriptions();
  
  void add(const MsgShortName& name, const int len, const SlowerRemote& remote );
  
  void remove(const MsgShortName& name, const int len, const SlowerRemote& remote );
  
  std::list<SlowerRemote> find(  const MsgShortName& name  ) ;
    
 private:
  std::vector< std::map<MsgShortName,std::set<SlowerRemote>> > subscriptions;

};


