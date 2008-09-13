#ifndef LINKTRACKERHH
#define LINKTRACKERHH

/*
 * =c
 * LinkTracker(TAU)
 * =s Grid
 * =d
 *
 * Expects Grid packets as input.  Extracts ping-ponged link stat info
 * from unicast packet headers and incorporates it into running
 * averages.  TAU is the time constant to use for the
 * exponential decay averaging, in milliseconds.
 *
 * =a
 * AiroInfo, LinkStat, PingPong */

#include <click/bighashmap.hh>
#include <click/element.hh>
#include <click/glue.hh>
#include <click/ipaddress.hh>
CLICK_DECLS

class LinkTracker : public Element {

  double _tau;

  static String read_stats(Element *, void *);
  static String read_bcast_stats(Element *, void *);
  static String read_tau(Element *, void *);
  static int write_tau(const String &, Element *, void *, ErrorHandler *);

  struct stat_t {
    double qual_top;
    double qual_bot;
    double sig_top;
    double sig_bot;
    Timestamp last_data; // when last data was generated by other end of link (other node's time)
    Timestamp last_update; // when we last updated the data (our time)
  };

  struct bcast_t {
    // broadcast delivery rate
    double r_top; 
    double r_bot;
    Timestamp last_bcast;
    Timestamp last_update;
  };

  HashMap<IPAddress, stat_t> _stats;
  HashMap<IPAddress, bcast_t> _bcast_stats;

public:
  void add_stat(IPAddress dst, int sig, int qual, Timestamp when);
  bool get_stat(IPAddress dst, int &sig, int &qual, Timestamp &last_update);
  void add_bcast_stat(IPAddress dst, unsigned int num_rx, unsigned int num_expected, Timestamp last_bcast);
  bool get_bcast_stat(IPAddress dst, double &delivery_rate, Timestamp &last_update);
  void remove_all_stats(IPAddress dst);
  
  LinkTracker();
  ~LinkTracker();
  
  const char *class_name() const		{ return "LinkTracker"; }
  const char *port_count() const		{ return PORTS_1_1; }
  const char *processing() const		{ return "a/a"; }
  
  void add_handlers();

  int configure(Vector<String> &, ErrorHandler *);
  int initialize(ErrorHandler *);

  Packet *simple_action(Packet *);
};

CLICK_ENDDECLS
#endif
