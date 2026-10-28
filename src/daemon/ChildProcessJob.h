#pragma once

// Makes every process this process starts end with it. On Windows the process joins a job object that ends
// all of its processes when the last handle to the job closes, which the system does when the process exits
// or crashes; children inherit the job, so none outlives it. It is the counterpart of the Linux
// PR_SET_PDEATHSIG the runners set, and covers sd-cli and llama-completion too. Returns false when the job
// could not be set up (the process then runs as before). A no-op returning true elsewhere.
bool endChildrenWithThisProcess();
