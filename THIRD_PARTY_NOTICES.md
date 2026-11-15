# Third-party notices

UCAL's license applies to its original covered code. Dependencies retain their
own licenses. Vendor runtimes and SDK material are not included in the source
repository. Binary packages include readable dependency credits and license
texts in `licenses/ThirdPartyNotices.txt`. The shared LGPL and GPL texts are
`LICENSE` and `licenses/GPL-3.0.txt`; they are not duplicated per dependency.

| Dependency | Use | License and source |
| --- | --- | --- |
| Qt 5.14.2 Core, Gui, Widgets, Network, Concurrent and selected plugins | Manager and software frontend UI/runtime | LGPL-3.0; [Qt source archives](https://download.qt.io/archive/qt/5.14/5.14.2/), [Qt license obligations](https://www.qt.io/development/open-source-lgpl-obligations) |
| Third-party components in the selected Qt build, including ANGLE and image/font/text libraries | Included in Qt libraries/plugins or companion runtime libraries | Their copyright notices and applicable terms are reproduced in the consolidated notice text |
| GCC / libgcc / libstdc++ runtime | MinGW-built Manager and frontend packages | GPL-3.0 with [GCC Runtime Library Exception 3.1](https://www.gnu.org/licenses/gcc-exception-3.1.html); [GCC sources](https://gcc.gnu.org/releases.html) |
| MinGW-w64 and winpthreads | Windows support and thread code linked into distributed binaries | Selected runtime notices; [MinGW-w64](https://www.mingw-w64.org/) |
| Microsoft Visual C++ runtime and Direct3D shader compiler | MSVC Broker/workers and Qt graphics support | Microsoft redistribution terms for the selected build tools/SDK; these files are separately licensed and do not become LGPL code |

UCAL links the selected Qt libraries dynamically. A distribution must allow use
of compatible modified Qt libraries and preserve the applicable LGPL notices and
rights. UCAL's independent-module permission applies only to UCAL's own covered
code; it does not waive Qt's or any other dependency's obligations.

A redistributor must supply corresponding source and build information for the
covered UCAL binaries, plus the dependency source/information required by their
licenses. A version label or a link to a repository's moving default branch is
not corresponding-source delivery for an exact distributed build. Package
assembly requires readable dependency notices generated from the selected build
inputs. Upstream attribution JSON, the build receipt, unrelated platforms and
developer-tool agreements are not user package assets.
