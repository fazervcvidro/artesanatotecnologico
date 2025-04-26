# artesanatotecnologico

- OBJETIVOS
    - LEVANTAR OS REQUISITOS
    - FAZER O CRONOGRAMA


PROJETO INICIAL - Estimador de CSI para Canais Wi-Fi
1o passo. averiguar se é possível realizar isso no esp 32 <- deu certo
2o passo. Testou script dos desenvolvedores esp e conseguiu capturar dados do canal da rede 
3o passo. Entender todos os campos e componentes e informações que o esp 32 pega
4o passo. Criar a base do banco de dados no sqlite e testar transformação das informações cruas em sql
5o passo. Entender como desenvolver o aplicativo em java, testar interação esp32-app em java-sqlite

Funções desejáveis:
- Configurar via aplicativo:
  * Tempo de escuta;
  * O ambiente, com possível armazenamento de ambientes configurados
 
  * Escolha de leitura de ambiente
  * - opções como leitura de estado da leitura ex:em movimento/parado/com chuva entre a transmissão e recepção



requisitos a determinar:
o que fará o firmware, o hardware, o software. 
esp 32 se comunica com o celular como, wifi, bluetooth, sinal de fumaça?
quantas comunicações ao mesmo tempo
como será o desenvolvimento do aplicativo em java
como será estruturado o banco de dados?



notas: quantas vezes por segundo o esp32 amostra/pega informações do canal
notas: integração externa no futuro



Para funcionamento do código da esp, necessário clonar o https://github.com/espressif/esp-csi em uma pasta acessar a pasta esp-csi/tree/master/examples/get-started/csi_recv_router/main e alterar o código main conforme o que temos aqui além de colocar o ip do servidor do código como o ip da sua máquina se for em momentos de teste, posteriormente abrir o ESP-IDF e colocar cd (caminho da pasta csi_recv_router acima), dar os seguintes comandos
idf.py fullclean / 
idf.py build / 
idf.py -p (Porta serial que pegou na sua máquina) flash /

