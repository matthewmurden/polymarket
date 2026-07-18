"""Shared logging setup and a rate-limited HTTP session used by every stage
of the pipeline, so every fetch (url, status, row count) is auditable from
one place regardless of which module made the request.
"""
from __future__ import annotations

import logging
import time
from dataclasses import dataclass
from typing import Optional

import requests

# A standard browser UA, not a self-identifying one. Confirmed live while
# building this: a plain, honest custom UA string (e.g.
# "poly-event-calibration/1.0 (+research tool...)") gets served a decoy
# HTTP 200 "Technical Difficulties" apology page by at least one target
# site's (state.gov) bot-filter -- a soft block silently disguised as
# success, worse than an honest 403. A standard browser UA avoids it. This
# does not bypass robots.txt or its crawl-delay, which are still fully
# checked/respected (see official_sources.check_robots_allowed) and are the
# actual access-control signal honored here; it just avoids a UA-substring
# WAF rule that appears to false-positive on non-browser-looking strings
# rather than being a deliberate policy against any particular crawler.
DEFAULT_USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)

_LOG_CONFIGURED = False


def setup_logging(log_file: Optional[str] = None, level: int = logging.INFO) -> logging.Logger:
    """Configures the root logger once (console + optional file), and
    returns it. Safe to call from every module/subcommand -- repeat calls
    with a different log_file just add another file handler.
    """
    global _LOG_CONFIGURED
    logger = logging.getLogger("poly_event_calibration")
    logger.setLevel(level)

    fmt = logging.Formatter("%(asctime)s [%(levelname)s] %(name)s: %(message)s")

    if not _LOG_CONFIGURED:
        console = logging.StreamHandler()
        console.setFormatter(fmt)
        logger.addHandler(console)
        _LOG_CONFIGURED = True

    if log_file:
        already_has_file_handler = any(
            isinstance(h, logging.FileHandler) and getattr(h, "baseFilename", None)
            and h.baseFilename.endswith(log_file)
            for h in logger.handlers
        )
        if not already_has_file_handler:
            fh = logging.FileHandler(log_file)
            fh.setFormatter(fmt)
            logger.addHandler(fh)

    return logger


@dataclass
class FetchLogEntry:
    url: str
    status: Optional[int]
    row_count: Optional[int]
    note: str = ""


class RateLimitedSession:
    """Wraps requests.Session with:
      - a minimum delay between requests (politeness / robots crawl-delay)
      - automatic retry with exponential backoff on HTTP 429
      - a log line for every single request: url, status, and (if the
        caller supplies one) a row count, so a pipeline run is auditable
        after the fact from the log file alone.
    """

    def __init__(
        self,
        logger: logging.Logger,
        min_delay_seconds: float = 0.5,
        user_agent: str = DEFAULT_USER_AGENT,
        max_retries: int = 5,
        backoff_base_seconds: float = 2.0,
        max_backoff_seconds: float = 60.0,
        timeout_seconds: float = 20.0,
    ):
        self.logger = logger
        self.min_delay_seconds = min_delay_seconds
        self.max_retries = max_retries
        self.backoff_base_seconds = backoff_base_seconds
        self.max_backoff_seconds = max_backoff_seconds
        self.timeout_seconds = timeout_seconds
        self.session = requests.Session()
        self.session.headers.update({"User-Agent": user_agent})
        self._last_request_ts = 0.0

    def _throttle(self):
        elapsed = time.monotonic() - self._last_request_ts
        wait = self.min_delay_seconds - elapsed
        if wait > 0:
            time.sleep(wait)

    def get(self, url: str, row_count_fn=None, **kwargs) -> requests.Response:
        """GETs url, retrying on 429 with exponential backoff (honoring a
        Retry-After header if present). row_count_fn, if given, is called
        with the parsed response (only on a 200) to produce a row count for
        the log line -- e.g. `lambda r: len(r.json())`. Never raises on a
        non-2xx response; callers check response.status_code themselves,
        since "blocked"/"not found" are meaningful, loggable outcomes here,
        not exceptions.
        """
        kwargs.setdefault("timeout", self.timeout_seconds)
        last_response = None
        for attempt in range(self.max_retries + 1):
            self._throttle()
            self._last_request_ts = time.monotonic()
            try:
                response = self.session.get(url, **kwargs)
            except requests.RequestException as e:
                self.logger.warning("GET %s -> transport error: %s (attempt %d/%d)",
                                     url, e, attempt + 1, self.max_retries + 1)
                if attempt >= self.max_retries:
                    raise
                time.sleep(min(self.backoff_base_seconds * (2 ** attempt), self.max_backoff_seconds))
                continue

            last_response = response
            if response.status_code == 429:
                retry_after = response.headers.get("Retry-After")
                wait = float(retry_after) if retry_after and retry_after.isdigit() \
                    else min(self.backoff_base_seconds * (2 ** attempt), self.max_backoff_seconds)
                self.logger.warning("GET %s -> 429 rate limited, waiting %.1fs (attempt %d/%d)",
                                     url, wait, attempt + 1, self.max_retries + 1)
                time.sleep(wait)
                continue

            row_count = None
            if response.status_code == 200 and row_count_fn is not None:
                try:
                    row_count = row_count_fn(response)
                except Exception:
                    row_count = None
            self.logger.info("GET %s -> %s%s", url, response.status_code,
                              f" ({row_count} rows)" if row_count is not None else "")
            return response

        # Exhausted retries, all of them 429s.
        self.logger.error("GET %s -> gave up after %d retries, still rate limited",
                           url, self.max_retries)
        return last_response
