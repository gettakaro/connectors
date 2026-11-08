"""Single source of truth for every exit code the command can produce."""

OK = 0
UNEXPECTED = 1
USAGE = 2
TARGET = 3
UPSTREAM = 4
INTEGRITY = 5
BUILD = 6
CONFLICT = 7
VERIFICATION = 8
TRACKER = 9
PUBLISH = 10
INTERRUPTED = 130

DESCRIPTIONS: dict[int, str] = {
    OK: "ok",
    UNEXPECTED: "unexpected error",
    USAGE: "usage, configuration or catalog invalid",
    TARGET: "target not found or ambiguous default",
    UPSTREAM: "upstream input or provider unavailable",
    INTEGRITY: "integrity mismatch",
    BUILD: "build failed",
    CONFLICT: "artifact, ledger or asset conflict",
    VERIFICATION: "verification failed or evidence missing",
    TRACKER: "tracker or GitHub failure, or missing auth",
    PUBLISH: "icon bucket read or upload failed",
    INTERRUPTED: "interrupted",
}


class MaintError(Exception):
    """An error that carries the exit code the command should end with."""

    def __init__(self, code: int, message: str, **detail: object) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.detail = detail


class UsageError(MaintError):
    def __init__(self, message: str, **detail: object) -> None:
        super().__init__(USAGE, message, **detail)


class TargetError(MaintError):
    def __init__(self, message: str, **detail: object) -> None:
        super().__init__(TARGET, message, **detail)


class UpstreamUnavailable(MaintError):
    def __init__(self, message: str, **detail: object) -> None:
        super().__init__(UPSTREAM, message, **detail)


class IntegrityError(MaintError):
    def __init__(self, message: str, **detail: object) -> None:
        super().__init__(INTEGRITY, message, **detail)


class BuildFailed(MaintError):
    def __init__(self, message: str, **detail: object) -> None:
        super().__init__(BUILD, message, **detail)


class ConflictError(MaintError):
    def __init__(self, message: str, **detail: object) -> None:
        super().__init__(CONFLICT, message, **detail)


class VerificationFailed(MaintError):
    def __init__(self, message: str, **detail: object) -> None:
        super().__init__(VERIFICATION, message, **detail)


class TrackerError(MaintError):
    def __init__(self, message: str, **detail: object) -> None:
        super().__init__(TRACKER, message, **detail)


class PublishFailed(MaintError):
    def __init__(self, message: str, **detail: object) -> None:
        super().__init__(PUBLISH, message, **detail)
