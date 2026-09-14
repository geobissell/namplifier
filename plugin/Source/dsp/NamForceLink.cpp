#include "container.h"
#include "wavenet/model.h"

namespace namplifier
{

// MSVC drops static architecture registrars from nam_core.lib unless a symbol
// from those TUs is referenced by the final binary.
struct NamParserForceLink
{
  NamParserForceLink()
  {
    nam::container::force_link_slimmable_container();
    nam::wavenet::force_link_wavenet();
  }
};

static NamParserForceLink namParserForceLink;

} // namespace namplifier
