"""regtable: host-side tools for the regtable register-table library.

    regtable gen      YAML -> registers.c/.h/.md + a typed Python client
    regtable connect  open a device and drop into a Python REPL with `dev`
    regtable watch    print register changes as they happen
    regtable fetch    the device as a chip: identity, table, SVD picks on the pins
    regtable serve    serve the Web Serial panel on localhost

The typed client runtime lives in regtable.client; generated clients
inherit from it. build_client() gives the same class straight from a
YAML file without writing files; RegtableClient.discover(transport)
builds one from the device's own table, no YAML at all.
"""

from .client import (RegtableClient, SerialTransport, PipeTransport,
                     RegtableError, TransportError, RemoteError,
                     SchemaDriftError, f32, check_value, schema_fingerprint)
from .gen import build_client, GenerationError

__version__ = "0.1.0"

__all__ = [
    "RegtableClient", "SerialTransport", "PipeTransport",
    "RegtableError", "TransportError", "RemoteError", "SchemaDriftError",
    "f32", "check_value", "schema_fingerprint", "build_client", "GenerationError", "__version__",
]
