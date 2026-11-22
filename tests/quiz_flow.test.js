const assert = require("node:assert/strict");
const fs = require("node:fs");

const frontend = fs.readFileSync("frontend/script.js", "utf8");
const server = fs.readFileSync("server.c", "utf8");
const engine = fs.readFileSync("quiz_engine.c", "utf8");

const submitStart = frontend.indexOf("function submitAnswer() {");
const submitEnd = frontend.indexOf("// -------------------- ANSWER FEEDBACK", submitStart);
assert(submitStart >= 0 && submitEnd > submitStart, "local answer submission must exist");
const submitBody = frontend.slice(submitStart, submitEnd);
assert(submitBody.includes("quizAnswers[currentQuestionIndex] = selectedAnswer"), "answers must be cached locally");
assert(!submitBody.includes("apiRequest("), "no backend request is allowed between questions");

const completeStart = frontend.indexOf("async function completeQuiz() {");
const completeEnd = frontend.indexOf("// -------------------- QUIZ RESULTS", completeStart);
assert(completeStart >= 0 && completeEnd > completeStart, "single completion request must exist");
const completeBody = frontend.slice(completeStart, completeEnd);
assert(completeBody.includes("/api/quiz/complete"), "all answers must be submitted once at completion");
assert(completeBody.includes("answers: quizAnswers"), "complete answer batch must be sent");

const nextStart = frontend.indexOf("function nextQuestion() {");
const nextEnd = frontend.indexOf("async function completeQuiz()", nextStart);
assert(nextStart >= 0 && nextEnd > nextStart, "local next-question navigation must exist");
assert(frontend.slice(nextStart, nextEnd).includes("quizQuestionCache[currentQuestionIndex + 1]"), "next questions must come from the preloaded cache");

assert(server.includes('"/api/quiz/complete"'), "server must expose batch completion endpoint");
assert(server.includes("persist_active_quiz_session"), "active sessions must be persisted");
assert(server.includes("restore_quiz_session"), "sessions must recover after process restart");
assert(server.includes("persist_completed_quiz_session"), "completion must be idempotently persisted");
assert(engine.includes("int quiz_engine_submit_answers("), "backend must score a complete answer batch");

console.log("Quiz flow tests: PASS");

const showScreenStart = frontend.indexOf("function showQuizScreen() {");
const showScreenEnd = frontend.indexOf("// -------------------- QUESTION OPTIONS", showScreenStart);
assert(showScreenStart >= 0 && showScreenEnd > showScreenStart, "quiz screen setup must exist");
const showScreenBody = frontend.slice(showScreenStart, showScreenEnd);
assert(!/\bquestionNumber\b/.test(showScreenBody), "showQuizScreen must not reference an out-of-scope questionNumber");
assert(!/\btotalQuestions\b/.test(showScreenBody), "showQuizScreen must not reference an out-of-scope totalQuestions");
assert(showScreenBody.includes("setVisible(quizScreen, true)"), "quiz screen must be shown before question rendering");
