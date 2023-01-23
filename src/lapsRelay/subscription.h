

#include <list>
#include <set>
#include <map>
#include <vector>

#include <laps.h>


class Subscriptions {
public:

  Subscriptions();
  
  void add(const PubName& name, const int len, const SlowerRemote& remote );
  
  void remove(const PubName& name, const int len, const SlowerRemote& remote );
  
  std::list<SlowerRemote> find(  const PubName& name  ) ;
    
 private:
  std::vector< std::map<PubName,std::set<SlowerRemote>> > subscriptions;

};


