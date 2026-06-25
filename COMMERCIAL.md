# Commercial Use

BlazeRules is source-available under the Functional Source License 1.1,
Apache 2.0 Future License (`FSL-1.1-ALv2`).

You may use, modify, and redistribute BlazeRules for permitted purposes under
the FSL. The license is intended to allow internal use, evaluation,
development, research, education, and professional services work for a licensed
customer, while preventing direct commercial free-riding.

## When You Need A Commercial License

Contact `licensing@blazerules.dev` for a commercial license if you want to:

- offer BlazeRules or substantially similar functionality as a commercial
  hosted service, managed service, SaaS, API, or platform;
- embed BlazeRules into a commercial product that competes with BlazeRules or
  exposes substantially similar rules-engine functionality to third parties;
- redistribute BlazeRules as part of an OEM product or commercial platform;
- sell hosted rule authoring, rule execution, governance, or decisioning
  services built around BlazeRules;
- receive enterprise support, certified builds, indemnity, custom licensing
  terms, or private/commercial features.

## BYOC And BYOL

BlazeRules is designed for BYOC deployments: the data plane runs in the
customer's cloud, VPC, Kubernetes cluster, or on-prem environment. Raw events do
not need to leave the customer's infrastructure.

For enterprise self-hosted deployments, BYOL means the customer brings a
commercial license key or commercial agreement for features and support that
are not covered by the public source-available license.

## SaaS Control Plane

The public/source-available repository is the local data plane: the engine,
SDK, local agent, local dashboard, examples, and supporting tools.

Hosted or enterprise control-plane features may be proprietary or commercially
licensed, including:

- hosted rule editor and schema-aware YAML tooling;
- approvals, comments, review workflows, and RBAC/SSO;
- centralized rule, lookup, and model registry;
- signed rule/model releases and deployment gates;
- fleet management, remote hot reload orchestration, and deployment status;
- audit logs, compliance exports, and governance reports;
- backtest orchestration and challenger/shadow-rule management;
- enterprise connectors, certified builds, support, and SLAs.

## Future Apache License

Each version of BlazeRules made available under `FSL-1.1-ALv2` also receives an
Apache License 2.0 grant that becomes effective two years after that specific
version is made available, as described in `LICENSE`.

This document is a practical summary, not a replacement for the license. The
license text in `LICENSE` controls.
