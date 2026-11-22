
// ======================================================
// QUIZMIND AI — FRONTEND JAVASCRIPT
// C BACKEND API STRUCTURE PRESERVED
// ======================================================

"use strict";

// -------------------- CONFIGURATION --------------------

const API_BASE = window.QUIZ_API_URL || "";

let quizSessionId = null;
let currentQuestion = null;
let selectedAnswer = null;
let quizStartTime = null;
let answerSubmitted = false;
let historyData = [];
let analyticsChart = null;
let selectedTopic = "AI & ML";

// -------------------- DOM HELPERS --------------------

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => document.querySelectorAll(selector);

function showToast(message) {
    const toast = $("#toast-message");

    if (!toast) {
        console.log(message);
        return;
    }

    toast.textContent = message;
    toast.classList.add("show");

    clearTimeout(showToast.timer);

    showToast.timer = setTimeout(() => {
        toast.classList.remove("show");
    }, 3000);
}

function setButtonLoading(button, loading, originalText = "") {
    if (!button) return;

    if (loading) {
        if (!button.dataset.originalText) {
            button.dataset.originalText = button.textContent;
        }

        button.disabled = true;
        button.textContent = "Please wait...";
    } else {
        button.disabled = false;
        button.textContent =
            originalText ||
            button.dataset.originalText ||
            button.textContent;
    }
}

function setVisible(element, visible) {
    if (!element) return;

    if (visible) {
        element.hidden = false;
        element.style.display = "";
    } else {
        element.hidden = true;
        element.style.display = "none";
    }
}

function formatDuration(seconds) {
    const total = Math.max(0, Number(seconds) || 0);
    const minutes = Math.floor(total / 60);
    const remainingSeconds = Math.floor(total % 60);

    return `${String(minutes).padStart(2, "0")}:${String(
        remainingSeconds
    ).padStart(2, "0")}`;
}

function formatDate(dateValue) {
    if (!dateValue) return "Recently";

    const date = new Date(dateValue);

    if (Number.isNaN(date.getTime())) {
        return String(dateValue);
    }

    return date.toLocaleDateString("en-US", {
        day: "numeric",
        month: "short",
        year: "numeric"
    });
}

function escapeHTML(value) {
    return String(value ?? "").replace(/[&<>"']/g, (character) => {
        const entities = {
            "&": "&amp;",
            "<": "&lt;",
            ">": "&gt;",
            '"': "&quot;",
            "'": "&#039;"
        };

        return entities[character];
    });
}

// -------------------- BACKEND CONNECTION --------------------

async function apiRequest(endpoint, options = {}) {
    const response = await fetch(`${API_BASE}${endpoint}`, {
        ...options,
        headers: {
            ...(options.headers || {})
        }
    });

    const responseText = await response.text();

    let data = {};

    if (responseText) {
        try {
            data = JSON.parse(responseText);
        } catch {
            data = { message: responseText };
        }
    }

    if (!response.ok) {
        throw new Error(
            data.message ||
            data.error ||
            `Server error: ${response.status}`
        );
    }

    return data;
}

async function checkBackendConnection() {
    const dot = $("#backend-status-dot");
    const statusText =
        $("#backend-status") ||
        $("#backend-status-text");

    try {
        await apiRequest("/");

        if (dot) {
            dot.classList.add("connected");
            dot.classList.remove("disconnected");
        }

        if (statusText) {
            statusText.textContent = "Backend Connected";
        }

        return true;
    } catch (error) {
        if (dot) {
            dot.classList.remove("connected");
            dot.classList.add("disconnected");
        }

        if (statusText) {
            statusText.textContent = "Backend Offline";
        }

        console.error("Backend connection failed:", error);
        return false;
    }
}

// -------------------- PAGE NAVIGATION --------------------

const viewMap = {
    "home": "home",
    "dashboard": "dashboard",
    "start quiz": "quiz",
    "start-quiz": "quiz",
    "quiz": "quiz",
    "leaderboard": "leaderboard",
    "quiz history": "history",
    "history": "history",
    "analytics": "analytics"
};

const pageIds = {
    dashboard: "dashboard-page",
    quiz: "start-quiz-page",
    analytics: "analytics-page",
    leaderboard: "leaderboard-page",
    history: "history-page"
};

function switchView(viewName) {
    const normalizedView = String(viewName || "home")
        .trim()
        .toLowerCase();

    const view = viewMap[normalizedView] || "home";

    const landing = $(".landing-experience");

    // Hide all app pages first.
    Object.values(pageIds).forEach((pageId) => {
        const page = document.getElementById(pageId);

        if (page) {
            page.classList.remove("active");
            page.style.display = "none";
        }
    });

    // Show homepage or requested page.
    if (view === "home") {
        if (landing) {
            landing.style.display = "";
        }
    } else {
        if (landing) {
            landing.style.display = "none";
        }

        const targetPage = document.getElementById(pageIds[view]);

        if (targetPage) {
            targetPage.style.display = "block";
            targetPage.classList.add("active");
        } else {
            console.error("Navigation page not found:", pageIds[view]);
        }
    }

    // Navigation active state.
    $$(".sidebar nav a").forEach((link) => {
        const label = link.textContent.trim().toLowerCase();
        const mappedView = link.hasAttribute("data-home-link")
            ? "home"
            : (viewMap[label] || "");

        link.classList.toggle("active", mappedView === view);
    });

    // Load page data.
    if (view === "dashboard") {
        loadDashboardData();
    }

    if (view === "history") {
        loadHistory();
    }

    if (view === "leaderboard") {
        loadLeaderboard();
    }

    if (view === "analytics") {
        loadAnalytics();
    }

    window.scrollTo({
        top: 0,
        behavior: "smooth"
    });
}

function setupNavigation() {
    const nav = $(".sidebar nav");
    const sidebar = $(".sidebar");
    const menuToggle = $(".mobile-nav-toggle");

    const closeMenu = () => {
        if (!sidebar || !menuToggle) return;
        sidebar.classList.remove("mobile-nav-open");
        document.body.classList.remove("mobile-menu-visible");
        menuToggle.setAttribute("aria-expanded", "false");
        menuToggle.setAttribute("aria-label", "Open navigation menu");
        const icon = menuToggle.querySelector(".mobile-nav-toggle-icon");
        if (icon) icon.classList.remove("is-open");
    };

    const openMenu = () => {
        if (!sidebar || !menuToggle) return;
        sidebar.classList.add("mobile-nav-open");
        document.body.classList.add("mobile-menu-visible");
        menuToggle.setAttribute("aria-expanded", "true");
        menuToggle.setAttribute("aria-label", "Close navigation menu");
        const icon = menuToggle.querySelector(".mobile-nav-toggle-icon");
        if (icon) icon.classList.add("is-open");
    };

    if (menuToggle && sidebar) {
        menuToggle.addEventListener("click", () => {
            const expanded = menuToggle.getAttribute("aria-expanded") === "true";
            if (expanded) closeMenu();
            else openMenu();
        });
        document.addEventListener("keydown", (event) => {
            if (event.key === "Escape") closeMenu();
        });
        document.addEventListener("click", (event) => {
            if (sidebar.classList.contains("mobile-nav-open") && !sidebar.contains(event.target)) {
                closeMenu();
            }
        });
    }

    if (nav && !nav.querySelector("[data-home-link]")) {
        const homeLink = document.createElement("a");
        homeLink.href = "#";
        homeLink.textContent = "Home";
        homeLink.dataset.homeLink = "true";
        nav.prepend(homeLink);
    }

    $$(".sidebar nav a").forEach((link) => {
        link.addEventListener("click", (event) => {
            event.preventDefault();
            closeMenu();
            if (link.hasAttribute("data-home-link")) {
                switchView("home");
                return;
            }
            const view = viewMap[link.textContent.trim().toLowerCase()];
            if (view) switchView(view);
        });
    });

    $$(".brand, .sidebar-brand").forEach((brand) => {
        brand.addEventListener("click", (event) => {
            event.preventDefault();
            closeMenu();
            switchView("home");
        });
    });

    $$("[data-open-quiz]").forEach((button) => {
        button.addEventListener("click", (event) => {
            event.preventDefault();
            closeMenu();
            switchView("quiz");
        });
    });

    $$("[data-open-view]").forEach((button) => {
        button.addEventListener("click", (event) => {
            event.preventDefault();
            closeMenu();
            switchView(button.dataset.openView);
        });
    });

    $$("[data-topic-shortcut]").forEach((card) => {
        card.addEventListener("click", () => {
            const topic = card.dataset.topicShortcut;
            if (topic) selectQuizTopic(topic);
            closeMenu();
            switchView("quiz");
        });
    });

    const returnButton = $("#return-dashboard-btn");
    if (returnButton) {
        returnButton.addEventListener("click", () => {
            resetQuiz();
            switchView("dashboard");
        });
    }
}

// -------------------- HISTORY DATA --------------------

function normalizeHistory(data) {
    if (Array.isArray(data)) return data;
    if (Array.isArray(data.history)) return data.history;
    if (Array.isArray(data.data)) return data.data;

    return [];
}

let historyRequestVersion = 0;

async function loadHistoryData() {
    const requestVersion = ++historyRequestVersion;

    try {
        const data = await apiRequest("/api/history", {
            cache: "no-store",
            headers: { "Cache-Control": "no-cache" }
        });
        const freshRecords = normalizeHistory(data);

        // Ignore an older response that arrived after a newer refresh.
        if (requestVersion === historyRequestVersion) {
            historyData = freshRecords;
        }

        return historyData;
    } catch (error) {
        console.error("History loading failed; retaining last confirmed records:", error);

        // A temporary network/API failure must never replace valid stats with zero.
        return historyData;
    }
}

function getRecordStats(record) {
    const correct = Number(record.score ?? 0);
    const total = Number(record.total ?? 0);

    const safeTotal = Math.max(0, total);
    const safeCorrect = Math.max(0, Math.min(correct, safeTotal));

    return {
        correct: safeCorrect,
        incorrect: Math.max(0, safeTotal - safeCorrect),
        total: safeTotal,
        percentage: safeTotal > 0
            ? (safeCorrect / safeTotal) * 100
            : 0,
        duration: Number(record.seconds ?? 0),
        student: record.name || "Student",
        topic: record.topic || "General",
        date: record.date || record.timestamp || ""
    };
}

// -------------------- DASHBOARD --------------------

async function loadDashboardData() {
    const records = await loadHistoryData();

    updateDashboardStats(records);
    renderRecentActivity(records);
}

function updateDashboardStats(records) {
    const totalQuizzes = records.length;

    let totalQuestions = 0;
    let totalCorrect = 0;
    let scoreSum = 0;

    records.forEach((record) => {
        const stats = getRecordStats(record);

        totalQuestions += stats.total;
        totalCorrect += stats.correct;
        scoreSum += stats.percentage;
    });

    const averageScore = totalQuizzes
        ? Math.round(scoreSum / totalQuizzes)
        : 0;

    const accuracy = totalQuestions
        ? Math.round((totalCorrect / totalQuestions) * 100)
        : 0;

    const values = {
        "#total-quizzes": totalQuizzes,
        "#total-questions": totalQuestions,
        "#average-score": `${averageScore}%`,
        "#ai-questions": totalQuestions,
        "#analytics-total-quizzes": totalQuizzes,
        "#analytics-total-questions": totalQuestions,
        "#analytics-average-score": `${averageScore}%`,
        "#analytics-accuracy": `${accuracy}%`
    };

    Object.entries(values).forEach(([selector, value]) => {
        const element = $(selector);

        if (element) {
            element.textContent = value;
        }
    });
}

function renderRecentActivity(records) {
    const container = $("#recent-activity");

    if (!container) return;

    if (!records.length) {
        container.innerHTML = `
            <div class="empty-state">
                <p>No quiz activity yet.</p>
                <span>Start your first quiz to see your progress here.</span>
            </div>
        `;

        return;
    }

    container.innerHTML = [...records].reverse().slice(0, 5)
        .map((record) => {
            const stats = getRecordStats(record);

            return `
                <div class="activity-item">
                    <div class="activity-info">
                        <strong>${escapeHTML(stats.topic)}</strong>
                        <span>
                            ${escapeHTML(stats.student)}
                            · ${escapeHTML(formatDate(stats.date))}
                        </span>
                    </div>

                    <div class="activity-score">
                        <strong>${Math.round(stats.percentage)}%</strong>
                        <span>${stats.correct}/${stats.total} correct</span>
                    </div>
                </div>
            `;
        }).join("");
}

// -------------------- QUIZ HISTORY --------------------

async function loadHistory() {
    const container = $("#history-table-body");
    const records = await loadHistoryData();

    if (!container) return;

    if (!records.length) {
        container.innerHTML = `
            <tr>
                <td colspan="5" class="empty-state">
                    No quiz history available yet.
                </td>
            </tr>
        `;

        return;
    }

    container.innerHTML = [...records].reverse().map((record) => {
        const stats = getRecordStats(record);

        return `
            <tr>
                <td>${escapeHTML(stats.student)}</td>
                <td>${escapeHTML(stats.topic)}</td>
                <td>${stats.correct}</td>
                <td>${stats.incorrect}</td>
                <td>
                    <strong>${Math.round(stats.percentage)}%</strong>
                    <span>${stats.correct}/${stats.total}</span>
                </td>
            </tr>
        `;
    }).join("");
}

// -------------------- LEADERBOARD --------------------

async function loadLeaderboard() {
    const container = $("#leaderboard-table-body");
    const records = await loadHistoryData();

    if (!container) return;

    if (!records.length) {
        container.innerHTML = `
            <tr>
                <td colspan="4" class="empty-state">
                    No leaderboard data available yet.
                </td>
            </tr>
        `;

        return;
    }

    const studentBest = new Map();

    records.forEach((record) => {
        const stats = getRecordStats(record);
        const key = stats.student.trim().toLowerCase();

        const existing = studentBest.get(key);

        if (!existing || stats.percentage > existing.percentage) {
            studentBest.set(key, {
                student: stats.student,
                topic: stats.topic,
                correct: stats.correct,
                total: stats.total,
                percentage: stats.percentage
            });
        }
    });

    const rankedStudents = [...studentBest.values()]
        .sort((a, b) => b.percentage - a.percentage);

    container.innerHTML = rankedStudents.map((student, index) => `
        <tr>
            <td>
                <strong class="leaderboard-rank-number">
                    #${index + 1}
                </strong>
            </td>

            <td>${escapeHTML(student.student)}</td>
            <td>${escapeHTML(student.topic)}</td>

            <td>
                <strong>${Math.round(student.percentage)}%</strong>
                <span>${student.correct}/${student.total}</span>
            </td>
        </tr>
    `).join("");
}

// -------------------- TOPIC SELECTION --------------------

function selectQuizTopic(topic) {
    selectedTopic = topic;

    $$(".quiz-topic-btn").forEach((button) => {
        const buttonTopic =
            button.dataset.topic || button.textContent.trim();

        button.classList.toggle(
            "selected",
            buttonTopic.toLowerCase() === topic.toLowerCase()
        );
    });

    const topicInput = $("#selected-topic");

    if (topicInput) {
        topicInput.value = topic;
    }

    const topicDisplay = $("#quiz-selected-topic");

    if (topicDisplay) {
        topicDisplay.textContent = topic;
    }
}

function setupTopicSelection() {
    $$(".quiz-topic-btn").forEach((button) => {
        button.addEventListener("click", () => {
            const topic =
                button.dataset.topic ||
                button.textContent.trim();

            selectQuizTopic(topic);
        });
    });
}

// -------------------- QUIZ SETTINGS --------------------

function getPlayerName() {
    const input =
        $("#student-name") ||
        $("#player-name") ||
        $("#quiz-player-name");

    return input?.value.trim() || "";
}

function getQuestionCount() {
    const input =
        $("#question-count") ||
        $("#quiz-question-count-input");

    const count = Number(input?.value || 10);

    return Math.max(1, Math.min(15, count));
}

// -------------------- START QUIZ --------------------

async function startQuiz() {
    const startButton = $("#start-quiz-btn");
    const player = getPlayerName();
    const questionCount = getQuestionCount();

    if (!player) {
        showToast("Please enter your name first.");
        $("#student-name")?.focus();
        return;
    }

    const activeTopic =
        $(".quiz-topic-btn.selected") ||
        $(".quiz-topic-btn.active");

    if (activeTopic) {
        selectedTopic =
            activeTopic.dataset.topic ||
            activeTopic.textContent.trim();
    }

    if (!selectedTopic) {
        selectedTopic = "General Knowledge";
    }

    setButtonLoading(startButton, true);

    try {
        console.log("Starting quiz:", {
            player,
            topic: selectedTopic,
            count: questionCount
        });

        // Original C backend API request preserved.
        const data = await apiRequest("/api/quiz/start", {
            method: "POST",
            headers: {
                "Content-Type": "application/json"
            },
            body: JSON.stringify({
                player: player,
                topic: selectedTopic,
                count: questionCount
            })
        });

        console.log("Quiz start response:", data);

        const receivedQuestion =
            data.current_question ||
            data.next_question ||
            data.question;

        if (!data.session_id || !receivedQuestion) {
            console.error("Unexpected backend response:", data);

            throw new Error(
                "Backend did not return a session ID and question."
            );
        }

        quizSessionId = data.session_id;
        currentQuestion = receivedQuestion;

        selectedAnswer = null;
        answerSubmitted = false;
        window.quizNextQuestion = null;

        quizStartTime = Date.now();

        // Make sure the quiz page itself is visible.
        switchView("quiz");

        showQuizScreen();
        renderQuestion(currentQuestion);

    } catch (error) {
        console.error("Quiz start failed:", error);
        showToast(error.message || "Quiz could not start.");
    } finally {
        setButtonLoading(startButton, false);
    }
}

// -------------------- QUIZ SCREEN --------------------

function showQuizScreen() {
    const settings = $("#quiz-settings-card");
    const quizScreen = $("#quiz-screen");
    const resultScreen = $("#quiz-result-screen");
    const infoPanel = $(".quiz-info-panel");

    setVisible(settings, false);
    setVisible(quizScreen, true);
    setVisible(resultScreen, false);

    if (infoPanel) {
        infoPanel.style.display = "none";
    }

    const feedback = $("#quiz-feedback");

    if (feedback) {
        feedback.innerHTML = "";
    }

    const nextButton = $("#next-question-btn");
    const submitButton = $("#submit-answer-btn");

    setVisible(nextButton, false);

    if (submitButton) {
        submitButton.disabled = true;
    }
}

// -------------------- QUESTION OPTIONS --------------------

function normalizeOptions(options) {
    if (!Array.isArray(options)) {
        return [];
    }

    const letters = ["A", "B", "C", "D"];

    return options.map((option, index) => {
        if (typeof option === "object" && option !== null) {
            return {
                letter: option.letter || option.key || letters[index],
                text:
                    option.text ||
                    option.value ||
                    option.option ||
                    String(option)
            };
        }

        let text = String(option);

        text = text.replace(/^[A-D][).:\-]\s*/i, "");

        return {
            letter: letters[index] || String(index + 1),
            text
        };
    });
}

// -------------------- QUESTION RENDERING --------------------

function renderQuestion(question) {
    if (!question) {
        showToast("Question data is missing.");
        return;
    }

    currentQuestion = question;
    selectedAnswer = null;
    answerSubmitted = false;

    const questionNumber = Number(question.number || 1);
    const totalQuestions = Number(
        question.total || getQuestionCount()
    );

    const counter = $("#quiz-question-counter");
    const progressText = $("#quiz-progress-text");
    const progressBar = $("#quiz-progress-bar");
    const questionText = $("#quiz-question");
    const optionsContainer = $("#quiz-options");
    const feedback = $("#quiz-feedback");
    const nextButton = $("#next-question-btn");
    const submitButton = $("#submit-answer-btn");

    if (counter) {
        counter.textContent =
            `QUESTION ${questionNumber} OF ${totalQuestions}`;
    }

    if (progressText) {
        progressText.textContent =
            `${questionNumber} / ${totalQuestions}`;
    }

    if (progressBar) {
        const percentage =
            (questionNumber / Math.max(1, totalQuestions)) * 100;

        progressBar.style.width = `${percentage}%`;
    }

    if (questionText) {
        questionText.textContent =
            question.question || "Question unavailable.";
    }

    if (feedback) {
        feedback.innerHTML = "";
    }

    setVisible(nextButton, false);

    if (submitButton) {
        submitButton.disabled = true;
    }

    if (!optionsContainer) {
        console.error("Quiz options container not found.");
        return;
    }

    const options = normalizeOptions(question.options);

    if (!options.length) {
        optionsContainer.innerHTML = `
            <p class="quiz-error">
                No options were received for this question.
            </p>
        `;

        return;
    }

    optionsContainer.innerHTML = options.map((option) => `
        <button
            type="button"
            class="quiz-option"
            data-answer="${escapeHTML(option.letter)}"
        >
            <span class="option-letter">
                ${escapeHTML(option.letter)}
            </span>

            <span class="option-text">
                ${escapeHTML(option.text)}
            </span>
        </button>
    `).join("");

    $$(".quiz-option").forEach((button) => {
        button.addEventListener("click", () => {
            if (answerSubmitted) return;

            selectAnswer(button.dataset.answer);
        });
    });

    updateSubmitButton();
}

// -------------------- SELECT ANSWER --------------------

function selectAnswer(answer) {
    if (answerSubmitted) return;

    selectedAnswer = answer;

    $$(".quiz-option").forEach((button) => {
        button.classList.toggle(
            "selected",
            button.dataset.answer === answer
        );
    });

    updateSubmitButton();
}

function updateSubmitButton() {
    const submitButton = $("#submit-answer-btn");

    if (!submitButton) return;

    submitButton.disabled = !selectedAnswer || answerSubmitted;
}

// -------------------- SUBMIT ANSWER --------------------

async function submitAnswer() {
    if (!quizSessionId) {
        showToast("Quiz session not found.");
        return;
    }

    if (!selectedAnswer) {
        showToast("Please select an answer.");
        return;
    }

    if (answerSubmitted) return;

    const submitButton = $("#submit-answer-btn");

    setButtonLoading(submitButton, true);

    try {
        // Original C backend API request preserved.
        const data = await apiRequest("/api/quiz/answer", {
            method: "POST",
            headers: {
                "Content-Type": "application/json"
            },
            body: JSON.stringify({
                session_id: quizSessionId,
                answer: selectedAnswer
            })
        });

        answerSubmitted = true;

        window.quizNextQuestion = data.next_question || null;

        showAnswerFeedback(data);

        if (data.completed) {
            showQuizResults(data);
        } else {
            const nextButton = $("#next-question-btn");
            setVisible(nextButton, true);
        }

    } catch (error) {
        console.error("Answer submission error:", error);
        showToast(error.message || "Unable to submit answer.");
    } finally {
        setButtonLoading(
            submitButton,
            false,
            "Submit answer ↗"
        );

        if (answerSubmitted && submitButton) {
            submitButton.disabled = true;
        }
    }
}

// -------------------- ANSWER FEEDBACK --------------------

function showAnswerFeedback(data) {
    const feedback = $("#quiz-feedback");

    if (!feedback) return;

    const correct = Boolean(data.correct);
    const correctAnswer = data.correct_answer || "";
    const explanation =
        data.explanation || "No explanation available.";

    feedback.innerHTML = `
        <div class="answer-feedback ${correct ? "correct" : "incorrect"}">
            <div class="feedback-heading">
                ${correct
                    ? "Correct answer!"
                    : "Not quite. Keep learning!"}
            </div>

            <p>
                Correct answer:
                <strong>${escapeHTML(correctAnswer)}</strong>
            </p>

            <p class="feedback-explanation">
                ${escapeHTML(explanation)}
            </p>
        </div>
    `;

    $$(".quiz-option").forEach((button) => {
        button.disabled = true;

        const letter = button.dataset.answer;

        if (
            letter &&
            correctAnswer &&
            letter.toLowerCase() ===
            String(correctAnswer).toLowerCase()
        ) {
            button.classList.add("correct");
        }

        if (letter === selectedAnswer && !correct) {
            button.classList.add("incorrect");
        }
    });

    const submitButton = $("#submit-answer-btn");

    if (submitButton) {
        submitButton.disabled = true;
    }
}

// -------------------- NEXT QUESTION --------------------

function nextQuestion() {
    if (!answerSubmitted) {
        showToast("Submit your answer first.");
        return;
    }

    const next = window.quizNextQuestion;

    if (!next) {
        showToast("The backend did not send the next question.");
        console.error("Missing next_question in answer response.");
        return;
    }

    window.quizNextQuestion = null;

    renderQuestion(next);
}

// -------------------- QUIZ RESULTS --------------------

function showQuizResults(data) {
    const quizScreen = $("#quiz-screen");
    const resultScreen = $("#quiz-result-screen");

    setVisible(quizScreen, false);
    setVisible(resultScreen, true);

    const percentage = Number(data.percentage || 0);

    const results = {
        "#result-percentage": `${Math.round(percentage)}%`,
        "#result-student": data.student || getPlayerName(),
        "#result-topic": data.topic || selectedTopic,
        "#result-correct": data.correct ?? 0,
        "#result-incorrect": data.incorrect ?? 0,
        "#result-total": data.total ?? 0,
        "#result-time": formatDuration(
            data.duration_seconds || 0
        )
    };

    Object.entries(results).forEach(([selector, value]) => {
        const element = $(selector);

        if (element) {
            element.textContent = value;
        }
    });

    loadDashboardData();
    loadAnalytics();

    if (data.history_saved) {
        showToast("Quiz completed. Your result has been saved.");
    } else {
        showToast("Quiz completed, but the result was NOT saved. Rankings and analytics will not update.");
        console.error("Quiz result was not saved; check QUIZ_HISTORY_FILE and Render persistent disk.");
    }
}

// -------------------- RESET QUIZ --------------------

function resetQuiz() {
    quizSessionId = null;
    currentQuestion = null;
    selectedAnswer = null;
    answerSubmitted = false;
    quizStartTime = null;
    window.quizNextQuestion = null;

    setVisible($("#quiz-settings-card"), true);
    setVisible($("#quiz-screen"), false);
    setVisible($("#quiz-result-screen"), false);

    const infoPanel = $(".quiz-info-panel");

    if (infoPanel) {
        infoPanel.style.display = "";
    }

    const feedback = $("#quiz-feedback");

    if (feedback) {
        feedback.innerHTML = "";
    }

    const options = $("#quiz-options");

    if (options) {
        options.innerHTML = "";
    }

    const submitButton = $("#submit-answer-btn");

    if (submitButton) {
        submitButton.disabled = true;
    }

    switchView("quiz");
}

// -------------------- ANALYTICS --------------------

async function loadAnalytics() {
    const records = await loadHistoryData();

    updateAnalytics(records);
    renderAnalyticsChart(records);
    renderTopicPerformance(records);
}

function updateAnalytics(records) {
    let totalQuestions = 0;
    let totalCorrect = 0;
    let scoreSum = 0;
    let bestScore = 0;

    records.forEach((record) => {
        const stats = getRecordStats(record);

        totalQuestions += stats.total;
        totalCorrect += stats.correct;
        scoreSum += stats.percentage;

        bestScore = Math.max(bestScore, stats.percentage);
    });

    const averageScore = records.length
        ? Math.round(scoreSum / records.length)
        : 0;

    const accuracy = totalQuestions
        ? Math.round((totalCorrect / totalQuestions) * 100)
        : 0;

    const values = {
        "#analytics-total-quizzes": records.length,
        "#analytics-total-questions": totalQuestions,
        "#analytics-average-score": `${averageScore}%`,
        "#analytics-best-score": `${Math.round(bestScore)}%`,
        "#analytics-accuracy": `${accuracy}%`,
        "#analytics-correct-answers": totalCorrect
    };

    Object.entries(values).forEach(([selector, value]) => {
        const element = $(selector);

        if (element) {
            element.textContent = value;
        }
    });
// -------------------- ANALYTICS PROGRESS CIRCLES --------------------

const circumference = 2 * Math.PI * 48;

function setProgressCircle(selector, percentage) {
    const circle = $(selector);

    if (!circle) return;

    const safePercentage = Math.max(
        0,
        Math.min(100, Number(percentage) || 0)
    );

    const progressLength =
        (safePercentage / 100) * circumference;

    // Start from 12 o'clock
    circle.setAttribute(
        "transform",
        "rotate(-90 60 60)"
    );

    // Set accurate progress length
    circle.style.strokeDasharray =
        `${progressLength} ${circumference}`;

    // Keep progress direction consistent
    circle.style.strokeDashoffset = "0";
}

// Average score
setProgressCircle("#average-circle", averageScore);

// Best score
setProgressCircle("#best-circle", bestScore);

    const insights = $("#performance-insights");

    if (insights) {
        insights.innerHTML = `
            <div class="analytics-overview-grid">
                <div class="analytics-overview-card">
                    <span>Total Quizzes</span>
                    <strong>${records.length}</strong>
                    <small>Completed sessions</small>
                </div>

                <div class="analytics-overview-card">
                    <span>Questions Attempted</span>
                    <strong>${totalQuestions}</strong>
                    <small>Across all quizzes</small>
                </div>

                <div class="analytics-overview-card">
                    <span>Average Score</span>
                    <strong>${averageScore}%</strong>
                    <small>Overall performance</small>
                </div>

                <div class="analytics-overview-card">
                    <span>Accuracy</span>
                    <strong>${accuracy}%</strong>
                    <small>Correct answers</small>
                </div>
            </div>
        `;
    }
}

function renderAnalyticsChart(records) {
    const canvas =
        $("#performance-trend-chart") ||
        $("#analytics-chart");

    if (!canvas || typeof Chart === "undefined") return;

    const recentRecords = records.slice(-10);
    const firstQuizNumber = Math.max(1, records.length - recentRecords.length + 1);
    const labels = recentRecords.map((record, index) =>
        `Quiz ${firstQuizNumber + index}`
    );
    const scores = recentRecords.map((record) =>
        Math.round(getRecordStats(record).percentage)
    );

    // A real rolling average smooths the visual trend without changing the source scores.
    const rollingAverage = scores.map((score, index) => {
        const windowScores = scores.slice(Math.max(0, index - 2), index + 1);
        return Math.round(
            windowScores.reduce((sum, value) => sum + value, 0) /
            windowScores.length
        );
    });

    if (analyticsChart) {
        analyticsChart.destroy();
    }

    analyticsChart = new Chart(canvas, {
        type: "line",
        data: {
            labels,
            datasets: [
                {
                    label: "Quiz score",
                    data: scores,
                    borderColor: "#345b48",
                    backgroundColor: "rgba(52, 91, 72, 0.10)",
                    borderWidth: 3,
                    fill: true,
                    tension: 0.35,
                    pointRadius: 5,
                    pointHoverRadius: 7
                },
                {
                    label: "3-quiz average",
                    data: rollingAverage,
                    borderColor: "#d39a45",
                    backgroundColor: "transparent",
                    borderWidth: 2,
                    borderDash: [6, 5],
                    fill: false,
                    tension: 0.35,
                    pointRadius: 2,
                    pointHoverRadius: 5
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            interaction: {
                mode: "index",
                intersect: false
            },
            plugins: {
                legend: {
                    display: true,
                    position: "bottom"
                },
                tooltip: {
                    callbacks: {
                        title: (items) => items.length ? items[0].label : ""
                    }
                }
            },
            scales: {
                y: {
                    beginAtZero: true,
                    max: 100,
                    ticks: {
                        callback: (value) => `${value}%`
                    }
                },
                x: {
                    grid: {
                        display: false
                    }
                }
            }
        }
    });
}

function renderTopicPerformance(records) {
    const container = $("#topic-performance");

    if (!container) return;

    if (!records.length) {
        container.innerHTML = `
            <p class="empty-state">
                Complete a quiz to see topic-wise performance.
            </p>
        `;

        return;
    }

    const topics = {};

    records.forEach((record) => {
        const stats = getRecordStats(record);
        const topic = stats.topic;

        if (!topics[topic]) {
            topics[topic] = {
                totalScore: 0,
                quizzes: 0
            };
        }

        topics[topic].totalScore += stats.percentage;
        topics[topic].quizzes++;
    });

    const sortedTopics = Object.entries(topics)
        .map(([topic, data]) => ({
            topic,
            average: Math.round(
                data.totalScore / data.quizzes
            ),
            quizzes: data.quizzes
        }))
        .sort((a, b) => b.average - a.average);

    container.innerHTML = sortedTopics.map((item) => `
        <div class="topic-performance-item">
            <div class="topic-performance-heading">
                <strong>${escapeHTML(item.topic)}</strong>
                <span>${item.average}%</span>
            </div>

            <div class="topic-performance-track">
                <div
                    class="topic-performance-fill"
                    style="width: ${item.average}%"
                ></div>
            </div>

            <small>
                ${item.quizzes}
                quiz${item.quizzes === 1 ? "" : "zes"}
            </small>
        </div>
    `).join("");
}

// -------------------- TOPIC SEARCH --------------------

function setupTopicSearch() {
    const searchInput =
        $("#global-topic-search") ||
        $("#topic-search") ||
        $("#global-search");

    if (!searchInput) return;

    searchInput.addEventListener("input", () => {
        const searchValue = searchInput.value
            .trim()
            .toLowerCase();

        $$(".story-topic-card, .topic-card, [data-topic-shortcut]")
            .forEach((card) => {
                const text = card.textContent.toLowerCase();

                card.style.display =
                    text.includes(searchValue) ? "" : "none";
            });
    });
}

// -------------------- REVEAL ANIMATIONS --------------------

function setupRevealAnimations() {
    const elements = $$(".reveal-section");

    if (!elements.length) return;

    if (
        !("IntersectionObserver" in window) ||
        window.matchMedia("(prefers-reduced-motion: reduce)").matches
    ) {
        elements.forEach((element) => {
            element.classList.add("visible");
        });

        return;
    }

    const observer = new IntersectionObserver(
        (entries) => {
            entries.forEach((entry) => {
                if (entry.isIntersecting) {
                    entry.target.classList.add("visible");
                }
            });
        },
        {
            threshold: 0.12
        }
    );

    elements.forEach((element) => {
        observer.observe(element);
    });
}

// -------------------- HERO VISUAL --------------------

function setupHeroVisual() {
    const canvas = $("#hero-canvas");

    if (!canvas || typeof THREE === "undefined") {
        return;
    }

    try {
        const scene = new THREE.Scene();

        const camera = new THREE.PerspectiveCamera(
            45,
            canvas.clientWidth / Math.max(canvas.clientHeight, 1),
            0.1,
            100
        );

        camera.position.z = 5;

        const renderer = new THREE.WebGLRenderer({
            canvas,
            alpha: true,
            antialias: true
        });

        renderer.setPixelRatio(
            Math.min(window.devicePixelRatio || 1, 2)
        );

        renderer.setSize(
            canvas.clientWidth,
            canvas.clientHeight
        );

        const geometry = new THREE.IcosahedronGeometry(1.3, 1);

        const material = new THREE.MeshStandardMaterial({
            color: "#345b48",
            roughness: 0.4,
            metalness: 0.12,
            wireframe: true
        });

        const shape = new THREE.Mesh(geometry, material);
        scene.add(shape);

        const light = new THREE.DirectionalLight("#ffffff", 2);
        light.position.set(2, 3, 4);
        scene.add(light);

        scene.add(new THREE.AmbientLight("#ffffff", 1));

        let animationFrame;

        function animate() {
            animationFrame = requestAnimationFrame(animate);

            shape.rotation.x += 0.002;
            shape.rotation.y += 0.003;

            renderer.render(scene, camera);
        }

        animate();

        window.addEventListener("resize", () => {
            const width = canvas.clientWidth;
            const height = canvas.clientHeight;

            if (!width || !height) return;

            camera.aspect = width / height;
            camera.updateProjectionMatrix();

            renderer.setSize(width, height);
        });

    } catch (error) {
        console.warn("Hero visual could not initialize:", error);
    }
}

// -------------------- QUIZ BUTTONS --------------------

function setupQuizButtons() {
    const startButton = $("#start-quiz-btn");
    const submitButton = $("#submit-answer-btn");
    const nextButton = $("#next-question-btn");

    if (startButton) {
        startButton.type = "button";

        startButton.addEventListener("click", (event) => {
            event.preventDefault();
            startQuiz();
        });
    }

    if (submitButton) {
        submitButton.type = "button";

        submitButton.addEventListener("click", (event) => {
            event.preventDefault();
            submitAnswer();
        });
    }

    if (nextButton) {
        nextButton.type = "button";

        nextButton.addEventListener("click", (event) => {
            event.preventDefault();
            nextQuestion();
        });
    }
}

// -------------------- INITIALIZATION --------------------

document.addEventListener("DOMContentLoaded", () => {
    console.log("QuizMind AI initialized.");

    const footerYear = $("#footer-year");

    if (footerYear) {
        footerYear.textContent = new Date().getFullYear();
    }

    setupNavigation();
    setupTopicSelection();
    setupQuizButtons();
    setupTopicSearch();
    setupRevealAnimations();
    setupHeroVisual();

    const firstTopic = $(".quiz-topic-btn");

    if (firstTopic) {
        selectQuizTopic(
            firstTopic.dataset.topic ||
            firstTopic.textContent.trim()
        );
    }

    // Start on homepage.
    switchView("home");

    // Backend connection and initial dashboard data.
    checkBackendConnection();
    loadDashboardData();
});
