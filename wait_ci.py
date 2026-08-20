import json
import urllib.request
import time
import sys

run_id = '32324802712'
url = f'https://api.github.com/repos/feifeigd/caf-plugin-system/actions/runs/{run_id}'

for i in range(30):
    try:
        with urllib.request.urlopen(url) as response:
            data = json.loads(response.read().decode())
            status = data['status']
            conclusion = data.get('conclusion', 'N/A')
            print(f'[{i}] status={status}, conclusion={conclusion}')
            if status == 'completed':
                if conclusion == 'success':
                    print('CI PASSED!')
                else:
                    print('CI FAILED!')
                    # Check artifacts
                    artifacts_url = data.get('artifacts_url', '')
                    if artifacts_url:
                        try:
                            with urllib.request.urlopen(artifacts_url) as ar:
                                ad = json.loads(ar.read().decode())
                                for a in ad.get('artifacts', []):
                                    print(f'  Artifact: {a["name"]} - {a["archive_download_url"]}')
                        except Exception as e:
                            print(f'  Artifact check error: {e}')
                sys.exit(0)
    except Exception as e:
        print(f'[{i}] error: {e}')
    time.sleep(20)

print('Timeout waiting for CI')
