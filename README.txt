AI QUIZ GAME - C PROJECT

FILES
- main.c: menu, gameplay, question bank, quiz history, leaderboard
- ai.c / ai.h: Gemini API request and background worker
- questions/: topic question banks (created automatically as needed)

REQUIREMENTS (WSL / Ubuntu)
Install compiler and libraries:
  sudo apt update
  sudo apt install build-essential libcurl4-openssl-dev libcjson-dev

Set your Gemini API key in the same terminal:
  export GEMINI_API_KEY="YOUR_API_KEY"

Compile from this project folder:
  gcc main.c ai.c -o quiz -lcurl -lcjson -pthread -Wall -Wextra

Run:
  ./quiz

The program creates/uses:
- questions/philosophy.json
- questions/ai_ml.json
- questions/history.json
- questions/c_programming.json
- questions/finance.json
- cache.json (temporary response cache)
- history.json (all completed attempts)

IMPORTANT
The Gemini model name is configured in ai.c. If your Google AI Studio account does not support
"gemini-3.5-flash-lite", replace that model name with a currently available model in your account.
The API request has a 20-second total timeout. AI generation runs in a background pthread;
the program joins the thread before using its result, so late responses are not used.
