#!/usr/bin/env bash
# Generate a submission report only after all required local checks pass.
set -euo pipefail

DOCX=true
while getopts ':mh' option; do
    case "$option" in
        m) DOCX=false ;;
        h) printf 'Usage: bash scripts/create-submission-report.sh [-m]\n  -m  Markdown only (pandoc is optional)\n'; exit 0 ;;
        *) printf 'Unknown option: -%s\n' "$OPTARG" >&2; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT_DIR="$(dirname -- "$SCRIPT_DIR")"
cd "$ROOT_DIR"
FINAL_REPORT="${FINAL_REPORT:-submission-report.md}"
FINAL_REPORT_OUTPUT="${FINAL_REPORT_OUTPUT:-submission-report.docx}"
if [[ "$DOCX" == true ]] && ! command -v pandoc >/dev/null; then
    printf 'pandoc is required for DOCX. Install pandoc or use -m for Markdown only.\n' >&2
    exit 1
fi
REPORT_TMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "$REPORT_TMP_DIR"' EXIT
REPORT_TMP_FILE="$REPORT_TMP_DIR/report.md"

{
    printf '# Submission Report\n\nGenerated: %s\n\nMachine: `%s`\n\n' "$(date -u '+%Y-%m-%d %H:%M:%S UTC')" "$(uname -a)"
    printf 'This report contains the README, actual build/test output, and project sources.\n\n'
    printf '## README\n\n'
    cat README.md
    printf '\n\n---\n\n'
} > "$REPORT_TMP_FILE"

capture_check() {
    local title="$1"
    shift
    local log="$REPORT_TMP_DIR/check.log"
    printf 'Running %s ...\n' "$*"
    if ! "$@" > "$log" 2>&1; then
        cat "$log" >&2
        printf 'Report generation stopped: %s failed.\n' "$*" >&2
        return 1
    fi
    {
        printf '## %s\n\nCommand: `%s`\n\n```text\n' "$title" "$*"
        sed 's/[[:blank:]]*$//' "$log"
        printf '\n```\n\n---\n\n'
    } >> "$REPORT_TMP_FILE"
}

capture_check 'Build Output' make all
capture_check 'Unit Test Results' make check
capture_check 'Coverage Report' make report
capture_check 'Application Address/Undefined Sanitizers' make leak
capture_check 'Unit Test Address/Undefined Sanitizers' make leak-test
capture_check 'UDP Transfer and I/O Edge Tests' make integration

{
    if [[ -f docs/benchmark-results.md ]]; then
        printf '## Performance Experiment\n\n'
        cat docs/benchmark-results.md
        printf '\n\n---\n\n'
    fi
    printf '## Project Sources\n\n'
    while IFS= read -r -d '' source; do
        case "$source" in
            *.py) language=python ;;
            *.sh) language=bash ;;
            *) language=c ;;
        esac
        printf '### %s\n\n```%s\n' "$source" "$language"
        cat "$source"
        printf '\n```\n\n'
    done < <(find src tests scripts -type f \( -name '*.c' -o -name '*.h' -o -name '*.py' -o -name '*.sh' \) \
        ! -path 'tests/harness/*' ! -name create-submission-report.sh -print0 | sort -z)
    printf '## End of Report\n\nAll commands above completed successfully.\n'
} >> "$REPORT_TMP_FILE"

mv -- "$REPORT_TMP_FILE" "$FINAL_REPORT"
if [[ "$DOCX" == true ]]; then
    pandoc --standalone "$FINAL_REPORT" -o "$FINAL_REPORT_OUTPUT"
    printf 'Created %s and %s\n' "$FINAL_REPORT" "$FINAL_REPORT_OUTPUT"
else
    printf 'Created %s\n' "$FINAL_REPORT"
fi
