import json, urllib.request

url = 'https://api.github.com/repos/feifeigd/caf-plugin-system/actions/runs?per_page=1'
with urllib.request.urlopen(url) as resp:
    d = json.loads(resp.read())['workflow_runs'][0]
    rid = d['id']
    print(f"Run #{d['run_number']}: {d['status']} / {d.get('conclusion', 'N/A')}")
    
    jobs_url = f'https://api.github.com/repos/feifeigd/caf-plugin-system/actions/runs/{rid}/jobs'
    with urllib.request.urlopen(jobs_url) as jresp:
        jdata = json.loads(jresp.read())
        for job in jdata.get('jobs', []):
            print(f"\nJob: {job['name']} - {job.get('conclusion', 'running')}")
            for step in job.get('steps', []):
                print(f"  Step {step['number']}: {step['name']} - {step.get('conclusion', 'running')}")
