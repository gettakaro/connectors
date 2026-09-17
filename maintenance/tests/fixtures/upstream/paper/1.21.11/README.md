# Paper 1.21.11 stand-ins

`paper-1.21.11-132.jar` is a ~400-byte zip, not the 54 MB server jar. The tests re-pin a
catalog copy to this file's own sha256 and serve it from an in-process HTTP server, so the
real download, integrity and failure paths run without shipping upstream bytes — and without
recording a hash of the real jar anywhere but the catalog record itself.
