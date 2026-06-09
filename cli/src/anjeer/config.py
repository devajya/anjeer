import json
import os
from dataclasses import dataclass, field, asdict
from pathlib import Path

_LOCAL_CONFIG = Path("anjeer.json")
_GLOBAL_CONFIG = Path.home() / ".anjeer" / "config.json"
CONFIG_PATH = _LOCAL_CONFIG if _LOCAL_CONFIG.exists() else _GLOBAL_CONFIG


@dataclass
class AnjeerConfig:
    api_key: str
    api_url: str
    ws_url: str
    script: str
    presets: dict = field(default_factory=dict)


def load_config() -> AnjeerConfig:
    if not CONFIG_PATH.exists():
        raise FileNotFoundError(
            f"Config not found at {CONFIG_PATH}. Run 'anjeer setup' first."
        )
    with open(CONFIG_PATH) as f:
        data = json.load(f)
    try:
        return AnjeerConfig(
            api_key=data["api_key"],
            api_url=data["api_url"].rstrip("/"),
            ws_url=data["ws_url"],
            script=data["script"],
            presets=data.get("presets", {}),
        )
    except KeyError as e:
        raise ValueError(f"Missing field {e} in {CONFIG_PATH}. Run 'anjeer setup' again.")


def save_config(cfg: AnjeerConfig) -> None:
    _GLOBAL_CONFIG.parent.mkdir(parents=True, exist_ok=True)
    with open(_GLOBAL_CONFIG, "w") as f:
        json.dump(asdict(cfg), f, indent=2)
