#ifndef XANADU_EVIDENCE_WINDOW_JOB_HPP
#define XANADU_EVIDENCE_WINDOW_JOB_HPP

#include <memory>
#include <optional>

#include "common/xanadu/evidence/window_composition.hpp"

namespace xanadu::evidence {

struct ObservationWindowCompletion {
  bool cancelled{};
  std::expected<vql::WindowResult, WindowCompositionError> result;
};

// The owner keeps case identity and lineage. Only bounded, owned evaluation
// inputs enter this worker; cancellation before polling prevents retention.
class ObservationWindowJob {
public:
  ObservationWindowJob();
  ~ObservationWindowJob();
  ObservationWindowJob(const ObservationWindowJob &)            = delete;
  ObservationWindowJob &operator=(const ObservationWindowJob &) = delete;

  bool start(ObservationWindowEvaluation evaluation);
  void cancel();
  [[nodiscard]] bool busy() const;
  [[nodiscard]] std::optional<ObservationWindowCompletion> poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu::evidence
#endif
