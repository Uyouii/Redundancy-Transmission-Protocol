import pymongo
import time


if __name__ == '__main__':

    csv_file_name = 'mrtp.csv'

    client = pymongo.MongoClient("localhost", 27017)
    db = client.NetworkLibraryTest

    data_set = {'rttData': []}

    with open(csv_file_name, 'r') as data_file:

        line = data_file.readline()
        while line:
            var_list = [var.strip() for var in line.split(',')]
            if var_list[0].isdigit():
                data_set['rttData'].append(var_list[1])
            else:
                data_set[var_list[0]] = var_list[1]

            line = data_file.readline()

    data_set['time'] = time.strftime("%Y-%m-%d")

    if data_set['library'] == 'mrtp':
        if data_set['packetStyle'] == 'redundancy':
            collection = db.mrtp_redundancy_test
        elif data_set['packetStyle'] == 'reliable':
            collection = db.mrtp_reliable_test
        elif data_set['packetStyle'] == 'unsequenced':
            collection = db.mrtp_unsequenced_test
        elif data_set['packetStyle'] == 'redundancynoack':
            collection = db.mrtp_redundancy_noack_test
        else:
            collection = db.other
    elif data_set['library'] == 'enet':
        collection = db.enet_test
    elif data_set['library'] == 'kcp':
        collection = db.kcp_test
    elif data_set['library'] == 'tcp':
        collection = db.tcp_test
    else:
        collection = db.other

    print data_set
    cursor = collection.find({'timeStamp': data_set['timeStamp']})
    if cursor.count() == 0:
        collection.insert(data_set)
        print "insert successfully"
    else:
        print "timeStamp is duplicated"
