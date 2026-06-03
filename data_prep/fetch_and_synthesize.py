import numpy as np
import pandas as pd
from datasets import load_dataset
import psycopg2
from psycopg2.extras import execute_values

def connect_db():
    return psycopg2.connect(
        host="localhost",
        database="benchmark_db",
        user="postgres",
        password="password",
        port="5432"
    )

def main():
    print("1. Hugging Face üzerinden MTEB veri setinin bir subset'i yükleniyor...")
    # Doğrulama aşaması için hafif ve 1536 boyutlu bir RAG seti seçiyoruz
    dataset = load_dataset("mteb/scidocs-reranking", split="test")
    
    # 100,000 vektörlük hedef subset'i simüle etmek için mevcut veriyi alıyoruz
    # Gerekirse veri miktarını ölçeklemek için döngüye sokabiliriz
    embeddings = []
    for item in dataset:
        if 'positive' in item and len(item['positive']) > 0:
            # Örnek bir embedding vektörü simülasyonu (1536 boyutlu)
            # Gerçek veriseti boyutuna göre truncate veya pad edilebilir
            vec = np.random.normal(0, 1, 1536)
            vec = vec / np.linalg.norm(vec) # Cosine similarity için normalize etme
            embeddings.append(vec.tolist())
        if len(embeddings) >= 100000:
            break
            
    N = len(embeddings)
    print(format(N, ',') + " adet 1536 boyutlu vektör başarıyla hazırlandı.")

    print("2. Rapor tanımlarına uygun sentetik meta veriler üretiliyor...")
    # Yüksek kardinalite: her satıra rastgele bir şirket/kiracı ID'si (1 - 5000)
    tenant_ids = np.random.randint(1, 5001, size=N)
    
    # Orta kardinalite: 100 farklı kategori ID'si (1 - 100)
    category_ids = np.random.randint(1, 101, size=N)
    
    # Düşük kardinalite: %80 false (0), %20 true (1) dağılım
    is_archived_flags = np.random.choice([False, True], size=N, p=[0.8, 0.2])

    print("3. PostgreSQL üzerinde hedef tablo şeması oluşturuluyor...")
    conn = connect_db()
    cur = conn.cursor()
    
    # Tabloyu sıfırla ve 1536 boyutlu vektör desteğiyle yeniden kur
    cur.execute("DROP TABLE IF EXISTS vector_benchmark;")
    cur.execute("""
        CREATE TABLE vector_benchmark (
            id SERIAL PRIMARY KEY,
            tenant_id INT,
            category_id INT,
            is_archived BOOLEAN,
            embedding vector(1536)
        );
    """)
    conn.commit()

    print("4. Veriler PostgreSQL'e binary COPY / toplu insert mantığıyla aktarılıyor...")
    # C++ tarafındaki Binary COPY performansına yakın hızlı bir veri aktarımı
    data_records = [
        (int(tenant_ids[i]), int(category_ids[i]), bool(is_archived_flags[i]), str(embeddings[i]))
        for i in range(N)
    ]
    
    query = "INSERT INTO vector_benchmark (tenant_id, category_id, is_archived, embedding) VALUES %s"
    execute_values(cur, query, data_records)
    conn.commit()
    
    cur.close()
    conn.close()
    print("Veri hazırlama adımı başarıyla tamamlandı. Tablo indekslenmeye hazır!")

if __name__ == "__main__":
    main()
