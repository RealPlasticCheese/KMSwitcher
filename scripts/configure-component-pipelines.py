"""Register component pipelines and variables without queueing builds or adding PR policies."""
import argparse
import json
from pathlib import Path
import subprocess
import shutil

ROOT = Path(__file__).resolve().parents[1]


def az(arguments, organization, project):
    executable = shutil.which('az.cmd') or shutil.which('az')
    result = subprocess.run([executable, *arguments, '--organization', organization,
                             '--project', project, '--output', 'json'],
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return json.loads(result.stdout) if result.stdout.strip() else None


def configure(branch, organization, project, repository):
    def azure(arguments):
        return az(arguments, organization, project)

    components = json.loads((ROOT / 'pipelines/components.json').read_text())
    existing = azure(['pipelines', 'list'])
    ids = {}
    for component, configuration in components.items():
        name = configuration['name']
        matches = [p for p in existing if p['name'] == name]
        path = 'azure-pipelines.yml' if component == 'catalogue' else f'pipelines/{component}.yml'
        if matches:
            pipeline = azure(['pipelines', 'update', '--id', str(matches[0]['id']), '--new-name', name,
                           '--branch', branch, '--yml-path', path])
        else:
            pipeline = azure(['pipelines', 'create', '--name', name, '--repository', repository,
                           '--repository-type', 'tfsgit', '--branch', branch, '--yml-path', path,
                           '--skip-first-run', 'true'])
        ids[component] = pipeline['id']
    for pipeline_id in ids.values():
        variables = azure(['pipelines', 'variable', 'list', '--pipeline-id', str(pipeline_id)]) or {}
        for component, definition_id in ids.items():
            name = 'KMS_' + component.upper() + '_PIPELINE_ID'
            mode = 'update' if name in variables else 'create'
            azure(['pipelines', 'variable', mode, '--pipeline-id', str(pipeline_id), '--name', name,
                '--value', str(definition_id), '--allow-override', 'false'])
    print(json.dumps(ids, indent=2))
    return ids


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--branch', default='master')
    parser.add_argument('--organization', required=True)
    parser.add_argument('--project', required=True)
    parser.add_argument('--repository', default='KMSwitcher')
    args = parser.parse_args()
    configure(args.branch, args.organization, args.project, args.repository)
