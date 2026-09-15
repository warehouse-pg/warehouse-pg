# AI Contribution Policy

WarehousePG (WHPG) is a complex project which users rely on to store
large amounts of analytical data. Maintaining the high standards required
for a production-grade PostgreSQL fork requires deep intent, architectural
alignment, and long-term maintainability.

While we recognize that AI-assisted tools (such as Gemini, Copilot, ChatGPT,
or Claude) can be powerful aids for development, they also facilitate
"low-effort" or "random" contributions that increase the burden on
maintainers without adding proportional value.

To protect the quality of the project and the sanity of its maintainers, we
have adopted the following policy regarding AI-generated content.

## 1. Human Accountability

**The human contributor is the sole party responsible for the contribution.**

If you submit a Pull Request that includes AI-generated code, documentation,
or comments:

- You must fully understand every line of code you submit.
- You must be able to explain the "why" behind the implementation during the
  review process.
- You are responsible for the long-term maintenance of that code.

"The AI generated it" is never an acceptable answer to a reviewer's question.
If a maintainer suspects you do not understand your own PR, it will be closed
immediately.

## 2. Intentionality and "Random" Contributions

WHPG is not a "testing ground" for AI experimentation. We do not accept PRs
that result from running an AI tool over the codebase to find "improvements"
without prior context or project alignment.

- **No "Shotgun" Refactoring:** Do not submit PRs that perform wide-scale
  refactoring or "clean-up" suggested by an AI unless specifically requested
  by a maintainer.
- **Design First:** For any non-trivial change, we strongly recommend opening
  an **Issue** or a **Discussion** first. PRs that arrive "out of the blue"
  with significant AI-generated logic — which do not align with our roadmap or
  architectural patterns — will be closed.
- **Quality over Quantity:** We value one thoughtful, manually crafted PR over
  ten AI-assisted "fixes" for non-existent or trivial issues.

## 3. Copyright and Legal

By submitting a contribution to WarehousePG, you represent and warrant that:

1. You have the legal right to submit the contribution under the project's
   license (Apache License 2.0).

2. The contribution does not violate the intellectual property rights of any
   third party.

3. If AI was used, the resulting code does not violate the terms of service of
   the AI provider and does not include "regurgitated" code from libraries
   with incompatible licenses (e.g., GPL-licensed code being suggested into
   our Apache-licensed project).

If you cannot guarantee the provenance and legal safety of the AI-generated
code, **do not submit it.**

## 4. Prohibited Uses

The following are strictly prohibited and will result in an immediate closure
of the PR and potentially a block from the organization:

- **Unvetted Boilerplate:** Submitting large blocks of AI-generated boilerplate
  that hasn't been trimmed to what is actually necessary.
- **Hallucinated Features:** Submitting PRs for features or bug fixes that
  don't exist, based on AI hallucinations about the project's capabilities.
- **Automated PR Descriptions:** Using AI to write PR descriptions that are
  vague, overly flowery, or fail to accurately describe the technical changes.
  We want to hear from *you*, the developer, why this change matters.

## 5. Disclosure

If AI was used to generate a significant portion of your contribution (beyond
simple autocomplete), we ask that you **disclose it** in the PR description and
through an `Assisted-by:` commit message trailer, as adopted by the
[OpenTelemetry project](https://github.com/open-telemetry/opentelemetry-collector/blob/main/AGENTS.md).

Transparency helps maintainers calibrate their review focus. Please add the
trailer to your commit message using the following format:

```
Assisted-by: Name of AI
```

Examples:

```
Assisted-by: ChatGPT 5.2
Assisted-by: Claude Opus 4.5
Assisted-by: Google Gemini
```

## 6. Enforcement

Maintainers reserve the right to close any PR that appears to be a "low-effort"
AI contribution without providing a detailed technical critique. Our time is
better spent supporting contributors who are deeply invested in the
CloudNativePG ecosystem.

---

### Acknowledgement

This policy was inspired by the [CloudNativePG Policy](https://github.com/cloudnative-pg/governance/blob/main/AI_POLICY.md),
the [Ghostty AI Policy](https://github.com/ghostty-org/ghostty/blob/main/AI_POLICY.md)
and the broader educational efforts of the CNCF and the Linux Foundation regarding
the responsible use of Generative AI in open-source development.

We particularly acknowledge the Linux Foundation’s [Generative AI Policy](https://www.linuxfoundation.org/legal/generative-ai)
and the CNCF’s initiatives to provide maintainers with frameworks for managing
the "machine-driven usage" of open-source infrastructure.
