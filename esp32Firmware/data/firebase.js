// ============ Firebase (sisi WEB) ============
// Tab Log membaca Firestore LANGSUNG dari browser via Firebase JS SDK dengan
// realtime listener (onSnapshot) — ESP32 hanya MENULIS (upload log saat selesai
// system run). Tabel log ter-update otomatis tanpa refresh saat ada data baru.
//
// Komunikasi ke app.js (script klasik) via window event, supaya urutan load
// module vs klasik tidak jadi masalah:
//   'fb-logs'   -> detail: array entry log (terbaru dulu)
//   'fb-status' -> detail: { ok, msg }
// dan helper global: window.fbLogsUpdate(id, {vehicle, plate}),
//                    window.fbLogsDeleteAll()

import { initializeApp } from 'https://www.gstatic.com/firebasejs/10.12.5/firebase-app.js';
import { getAuth, signInWithEmailAndPassword } from 'https://www.gstatic.com/firebasejs/10.12.5/firebase-auth.js';
import { getFirestore, collection, query, orderBy, limit, onSnapshot,
         doc, updateDoc, getDocs, deleteDoc } from 'https://www.gstatic.com/firebasejs/10.12.5/firebase-firestore.js';

const firebaseConfig = {
  apiKey: "AIzaSyA_eSYwlhtGTVbXZ5QJ6GlNKLeQfm5esRo",
  authDomain: "bayulogdatabase.firebaseapp.com",
  projectId: "bayulogdatabase",
  storageBucket: "bayulogdatabase.firebasestorage.app",
  messagingSenderId: "364763065826",
  appId: "1:364763065826:web:7d566262e60c8030421dbb",
};
const FB_EMAIL    = 'admin@gmail.com';
const FB_PASSWORD = 'admin123';
const LOGS_LIMIT  = 100;   // jumlah entri terbaru yang di-subscribe

const app  = initializeApp(firebaseConfig);
// PENTING: database project ini ber-ID 'default' (bukan '(default)' standar)
const db   = getFirestore(app, 'default');
const auth = getAuth(app);

function emitStatus(ok, msg) {
  window.dispatchEvent(new CustomEvent('fb-status', { detail: { ok, msg } }));
}
function emitLogs(arr) {
  window.dispatchEvent(new CustomEvent('fb-logs', { detail: arr }));
}

async function start() {
  try {
    await signInWithEmailAndPassword(auth, FB_EMAIL, FB_PASSWORD);
    emitStatus(true, 'auth ok');
    const q = query(collection(db, 'logs'), orderBy('created', 'desc'), limit(LOGS_LIMIT));
    onSnapshot(q, snap => {
      const arr = [];
      snap.forEach(d => {
        const v = d.data();
        arr.push({
          id: d.id,
          ts: v.ts || '', vehicle: v.vehicle || '', plate: v.plate || '',
          idx: v.idx_label || '', th_hc: v.th_hc ?? 0, th_co: v.th_co ?? 0,
          avg_hc: v.avg_hc ?? 0, avg_co: v.avg_co ?? 0,
          code: v.code ?? 0, n: v.n ?? 0, label: v.label || '',
          s_hc: v.s_hc || [], s_co: v.s_co || [],
        });
      });
      emitLogs(arr);
    }, err => {
      console.error('[FB] snapshot error:', err);
      emitStatus(false, err.code || 'snapshot error');
    });
  } catch (e) {
    console.error('[FB] auth error:', e);
    emitStatus(false, e.code || 'auth error');
  }
}
start();

// Edit vehicle/plate satu dokumen (dipakai modal detail di app.js)
window.fbLogsUpdate = (id, data) => updateDoc(doc(db, 'logs', id), data);

// Hapus semua dokumen log (batch per 200)
window.fbLogsDeleteAll = async () => {
  for (let round = 0; round < 10; round++) {
    const snap = await getDocs(query(collection(db, 'logs'), limit(200)));
    if (snap.empty) return;
    await Promise.all(snap.docs.map(d => deleteDoc(d.ref)));
  }
};
