# R1 educational writer review

## Scope and result

Reviewed the complete current `docs/vagent-user-guide.md` as a teaching document, including its
status framing, reading path, tutorials, reference sections, runtime traces, workshop and
troubleshooting material. The guide is unusually careful to distinguish what `check` verifies from
future runtime behavior; this is a strong teaching choice for a language whose executor is not
available yet.

**Convergence: not yet.** No must-fix blocker emerged, but the review perspectives converge on two
small, actionable improvements to help learners enter and practice the material. These are optional
polish, not requests to expand the course or require runtime execution.

## Findings

### Optional: state the assumed learner background

**Evidence:** `Implementation status`, `Build and check workflow`, and `Your first source file`;
especially “Follow the README setup instructions for the compiler and system dependencies” and the
first instruction to save a source unit and invoke a shell command.

**Learner impact:** The guide is clear about the toolchain but leaves readers to infer whether it is
for people new to programming, programmers new to agents, or contributors already comfortable with
the repository. A reader without shell/Make familiarity may mistake setup friction for a vagent
conceptual prerequisite, while an experienced language implementer may spend time on basic command
details.

**Concrete fix:** Add a short “Who this guide is for” note near the introduction: assume familiarity
with variables, functions, types, and a command line; no prior actor-language or logic-programming
knowledge is required. Link repository setup separately from the conceptual path. If the intended
audience differs, name that audience and the corresponding prerequisites instead.

### Optional: give the first checkpoint a small retrieval gap

**Evidence:** `Checkpoint: change one handler` asks readers to predict the count “then compare with
the answer: it stays at 3,” and then immediately gives the explanation. This is the first deliberate
practice point after the initial source file.

**Learner impact:** The task has a useful prediction prompt, but placing its answer in the same
sentence makes it easy to read passively. With runtime behavior explicitly predicted throughout the
guide, learners especially benefit from making a prediction before seeing the guide's trace.

**Concrete fix:** Separate the action from the answer with compact “Try” and “Check” labels (or move
the expected result to the end of the checkpoint). Preserve the current explanation that the
negative input skips the update rather than failing the handler. This requires no extra exercise or
runtime harness.

## Perspective convergence

- **Technical writer:** The status labels, repeated source/runtime distinction, and reading path set
  accurate expectations. State the audience once so setup instructions and the depth of the guide
  have a clear frame.
- **Programming teacher:** The small edits and expected results make the material practice-oriented;
  withholding the checkpoint result for a few lines would make the prediction an actual retrieval
  step.
- **Programming-language student:** The guide explains unfamiliar semantics with concrete examples
  and boundaries. A one-sentence prerequisite statement would help distinguish assumed programming
  knowledge from the new language concepts.

These perspectives agree on the two optional fixes above. They found no pedagogical issue that
requires redesigning the guide, adding runtime claims, or expanding it into a full course.
