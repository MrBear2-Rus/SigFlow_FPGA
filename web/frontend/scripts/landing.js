// 落地页：产品介绍 + 登录注册

import { api, ApiError } from "./lib/api.js";
import { initTabs } from "./lib/tabs.js";
import { toast } from "./lib/toast.js";

const $ = (id) => document.getElementById(id);
const NEXT_PAGE = "/pages/projects.html";

initTabs();

function setError(id, message) {
    $(id).textContent = message ?? "";
}

function setBusy(form, busy) {
    for (const button of form.querySelectorAll("button")) button.disabled = busy;
}

function showSignedIn(userId) {
    $("authForms").classList.add("hidden");
    $("signedIn").classList.remove("hidden");
    $("signedUser").textContent = `当前用户：${userId}`;

    const nav = $("navActions");
    nav.textContent = "";
    const enter = document.createElement("a");
    enter.className = "btn btn-primary";
    enter.href = NEXT_PAGE;
    enter.textContent = "进入工程";
    nav.append(enter);
}

function showSignedOut() {
    $("authForms").classList.remove("hidden");
    $("signedIn").classList.add("hidden");
}

async function login(userId, password) {
    await api.post("/api/session", { userId, password });
}

async function onLogin(event) {
    event.preventDefault();
    const form = event.currentTarget;
    setError("login-error", "");
    const userId = $("login-user").value.trim();
    const password = $("login-pass").value;
    if (!userId || !password) {
        setError("login-error", "请输入用户名和密码");
        return;
    }
    setBusy(form, true);
    try {
        await login(userId, password);
        location.href = NEXT_PAGE;
    } catch (error) {
        setError(
            "login-error",
            error instanceof ApiError && error.status === 401
                ? "用户名或密码错误"
                : error.message,
        );
    } finally {
        setBusy(form, false);
    }
}

async function onRegister(event) {
    event.preventDefault();
    const form = event.currentTarget;
    setError("register-error", "");
    const userId = $("register-user").value.trim();
    const password = $("register-pass").value;
    const confirm = $("register-confirm").value;

    if (!userId) return setError("register-error", "请输入用户名");
    if (password.length < 8) return setError("register-error", "密码至少 8 位");
    if (password !== confirm) return setError("register-error", "两次密码不一致");

    setBusy(form, true);
    try {
        await api.post("/api/users", { userId, password });
        await login(userId, password);
        location.href = NEXT_PAGE;
    } catch (error) {
        if (error instanceof ApiError && error.status === 409) {
            setError("register-error", "用户名已存在");
        } else {
            setError("register-error", error.message);
        }
    } finally {
        setBusy(form, false);
    }
}

async function onLogout() {
    try {
        await api.del("/api/session");
    } catch {
        /* 忽略 */
    }
    showSignedOut();
    const nav = $("navActions");
    nav.textContent = "";
    const loginButton = document.createElement("button");
    loginButton.className = "btn btn-ghost";
    loginButton.textContent = "登录";
    loginButton.addEventListener("click", () => selectTab("login"));
    const registerButton = document.createElement("button");
    registerButton.className = "btn btn-primary";
    registerButton.textContent = "注册";
    registerButton.addEventListener("click", () => selectTab("register"));
    nav.append(loginButton, registerButton);
    toast("已退出登录");
}

function selectTab(name) {
    const tab = document.querySelector(`[data-tabs] .tab[data-tab="${name}"]`);
    tab?.click();
    $("auth").scrollIntoView({ behavior: "smooth", block: "center" });
}

$("loginForm").addEventListener("submit", onLogin);
$("registerForm").addEventListener("submit", onRegister);
$("logout").addEventListener("click", onLogout);

for (const trigger of document.querySelectorAll("[data-auth]")) {
    trigger.addEventListener("click", (event) => {
        if (trigger.tagName === "A") event.preventDefault();
        selectTab(trigger.dataset.auth);
    });
}

api
    .get("/api/session")
    .then((session) => showSignedIn(session.userId))
    .catch(() => showSignedOut());
