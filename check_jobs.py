import json
import urllib.request

url = 'https://api.github.com/repos/feifeigd/caf-plugin-system/actions/runs/32324802712/jobs'
with urllib.request.urlopen(url) as response:
    data = json.loads(response.read().decode())
    for job in data.get('jobs', []):
        print(f"Job: {job['name']}, status={job['status']}, conclusion={job.get('conclusion', 'N/A')}")
        for step in job.get('steps', []):
            print(f"  Step {step['number']}: {step['name']} - status={step['status']} - conclusion={step.get('conclusion', 'N/A')}")
