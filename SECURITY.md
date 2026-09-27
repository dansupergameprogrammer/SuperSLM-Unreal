# Security Policy

## Supported versions

Only the latest tagged release is supported. Security fixes are not backported to older
releases.

## Reporting a vulnerability

Report a suspected vulnerability privately, through GitHub's Security Advisory feature for this
repository (**Security > Advisories > Report a vulnerability**). Do not open a public issue for
it.

Include:

- the affected version: the plugin's `VersionName` from `Plugins/SuperSLMUnreal/SuperSLMUnreal.uplugin`,
  and the SuperSLM tag and commit from
  `Plugins/SuperSLMUnreal/Source/ThirdParty/SuperSLM/VENDORED_VERSION.txt`;
- steps to reproduce, and the impact you assess.

A report that concerns SuperSLM's own code (the vendored runtime, the converter, or the `.sslm`
format) may be reported here or to the SuperSLM repository; the fix lands upstream and reaches
this plugin through a re-pin.

Model artifacts are loaded from files you choose. The importer validates an artifact's structure
and hash before it is used, but a model's output is only as trustworthy as the model: do not treat
generated text as trusted input to anything that executes it.

You will get an acknowledgement within a reasonable time, and a fix or a mitigation plan once the
report is triaged.
