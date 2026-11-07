//! Queue names in Cyrillic whose full path runs past Windows' 260-character
//! limit, written and read back across a restart in both persistence modes.

use std::time::Duration;

use bytes::Bytes;
use normfs::{NormFS, NormFsSettings, Persist, QueueSettings, ReadPosition};
use tokio::sync::mpsc;
use tokio::time::timeout;
use uintn::UintN;

const RECORDS: u64 = 100;

fn queue_name() -> String {
    // 200 characters, under NTFS's 255 per name, so only the whole path is long.
    let child: String = "дочерний-".chars().cycle().take(200).collect();
    format!("тестовая-папка/данные-кириллицей/{child}")
}

fn settings(persist: Persist) -> NormFsSettings {
    NormFsSettings {
        queue_settings: QueueSettings::all_active().with_default_persist(persist),
        ..NormFsSettings::all_active()
    }
}

fn record(i: u64) -> Bytes {
    Bytes::from(format!("запись-{i:06}-{}", "x".repeat(200)))
}

async fn round_trip(persist: Persist) {
    let temp = tempfile::TempDir::new().unwrap();
    let name = queue_name();
    let instance = {
        let fs = NormFS::new(temp.path().to_path_buf(), settings(persist))
            .await
            .unwrap();
        let queue = fs.resolve(&name);
        fs.ensure_queue_exists_for_write(&queue).await.unwrap();
        for i in 0..RECORDS {
            fs.enqueue(&queue, record(i)).await.unwrap();
        }
        let instance = fs.get_instance_id().to_string();
        fs.close().await.unwrap();
        instance
    };

    let dir = temp
        .path()
        .join(&instance)
        .join(name.replace('/', std::path::MAIN_SEPARATOR_STR));
    assert!(
        dir.as_os_str().len() > 260,
        "the queue directory must be longer than MAX_PATH to test anything"
    );
    let top: Vec<String> = std::fs::read_dir(temp.path().join(&instance))
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    assert!(
        top.iter().any(|n| n == "тестовая-папка"),
        "on disk: {top:?}"
    );

    let fs = NormFS::new(temp.path().to_path_buf(), settings(persist))
        .await
        .unwrap();
    let queue = fs.resolve(&name);
    fs.ensure_queue_exists_for_read(&queue).await.unwrap();
    let (tx, mut rx) = mpsc::channel(RECORDS as usize);
    fs.read(
        &queue,
        ReadPosition::Absolute(UintN::zero()),
        RECORDS,
        1,
        tx,
    )
    .await
    .unwrap();
    for i in 0..RECORDS {
        let entry = timeout(Duration::from_secs(10), rx.recv())
            .await
            .unwrap_or_else(|_| panic!("record {i} did not come back"))
            .expect("the stream ended early");
        assert_eq!(entry.data, record(i), "record {i}");
    }
    fs.close().await.unwrap();
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_long_cyrillic_queue_path_survives_a_restart_in_wal_and_store() {
    round_trip(Persist::WAL_STORE).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_long_cyrillic_queue_path_survives_a_restart_in_store_only() {
    round_trip(Persist::STORE).await;
}
