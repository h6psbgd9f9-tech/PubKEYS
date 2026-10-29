# Optional Telegram and Signal notifications

All delivery credentials and recipients are empty in the public source defaults.
No account belongs to or is supplied by the project. Each user configures their
own local settings. The application works without notification services.

## Telegram

Create your own bot using Telegram's BotFather and obtain the intended chat ID.
Set these fields in your local `config.ini`:

```ini
tg_token=
tg_chat=
tg_progress=0
```

Fill `tg_token` and `tg_chat` locally to enable delivery. Keep them blank to
disable it. Alternatively, provide defaults using the environment variables
`PUBKEYHUNT_TG_TOKEN` and `PUBKEYHUNT_TG_CHAT`. An explicit value read from
`config.ini`, including an empty value, can override an environment default;
omit those INI entries when deliberately relying on environment defaults.

`tg_progress=1` enables the configured periodic reports. The `progress` and
`check` intervals control speed and sample-report scheduling; console status
has its own periodic update.

## Signal

Signal integration uses **signal-cli**, installed and registered or linked by
the user. It does not use a Telegram-style bot API token.

```ini
signal_account=
signal_to=
signal_cli=signal-cli
signal_progress=0
```

Set the recipient in `signal_to`; supply `signal_account` when required to
select the locally configured signal-cli account. Set `signal_cli` to the
executable path if it is not on PATH. Leave `signal_to` blank to disable Signal.
The program does not bundle signal-cli credentials or account-registration data.

## What is delivered

Confirmed-finding notifications contain finding metadata. The actual confirmed
private key, WIF values, key card and archive stay in local result files.
Configured periodic sample reports can include a full trial private-key block
with public-key coordinates. Configure only recipients you intend to receive
those reports. Saving a confirmed match locally does not require an external
notification service to be available.

The `--check-notifications` command sends real test messages to configured
recipients. It is an explicit user diagnostic, not a build or CI step.

Local INI files, logs, Found files, checkpoints and GUI sessions are ignored by
Git. Do not add them with `git add -f`. No personal delivery setting is needed
in the repository or in GitHub Actions secrets for building the program.
