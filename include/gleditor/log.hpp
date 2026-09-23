#ifndef GLEDITOR_LOG_H
#define GLEDITOR_LOG_H

#include <ostream>

class Loggable {
protected:
  virtual void print(std::ostream &ost) const { ost << "LOG ME!!!"; };

public:
  Loggable()                            = default;
  virtual ~Loggable()                   = default;
  Loggable(const Loggable &)            = default;
  Loggable &operator=(const Loggable &) = default;
  Loggable(Loggable &&)                 = default;
  Loggable &operator=(Loggable &&)      = default;

  friend std::ostream &operator<<(std::ostream &ost, const Loggable &oth) {
    oth.print(ost);
    return ost;
  };
};

#endif // GLEDITOR_LOG_H
