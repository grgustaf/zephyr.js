// Copyright (c) 2016, Intel Corporation.

// Sample server that works with the linux server found in iotivity-constrained
// This sample will find the resource, retrieve its properties, and then update
// the resource with many different property types to test the conversion from
// JS to iotivity-constrained

var client = require('ocf').client;

print("Started OCF client");

client.on('error', function(error) {
  if (error.deviceId)
    print("Error for device: " + error.deviceId);
});

function onupdate(resource) {
    print("Resource updated:");
    print("    deviceId: " + resource.deviceId);
    print("    resourcePath: " + resource.resourcePath);
    if (resource.properties != undefined) {
        print("Resource has properties");
    } else {
        print("resource.properties not found");
    }
}

function found(resource) {
    print("Resource found: path=" + resource.resourcePath + " id=" + resource.deviceId);

    client.retrieve(resource.deviceId, {observable: false}, onupdate).then(function(res) {
        print("retrieve() was successful, deviceId=" + res.deviceId);
    }).catch(function(error) {
        print("retrieve() returned an error: " + error.name);
    });

    setTimeout(function() {
        resource.stringProp = "a string";
        resource.numProp = 123.456;
        resource.boolProp = true;
        resource.intProp = -1234;
        resource.uintProp = 1234;
        resource.stringProp2 = "another string";
        resource.objProp = {
            objIntProp: 111,
            objStringProp: "inside object",
            internalObjProp: {
                internalInt: 222,
                internalBool: false,
                internalString: "internal stringssss"
            }
        };
        resource.anotherObj = {
            anotherInt: 333,
            anotherString: "another string"
        };
        resource.array = ["test1", "test2", "test3", "test4", "test5"];
        resource.intArray = [1, 2, 3, 4, 5, 6];

        client.update(resource).then(function(resource) {
            print("update successful");
        }).catch(function(error) {
            print("Error updating name='" + error.name + "' message='" + error.message + "' " + "code=" + error.errorCode);
        });
    }, 1000);
}

client.findResources({resourceType:"oic.r.light"}, found).then(function(resource) {
    print("findResources() was successful, deviceId=" + resource.deviceId);
}).catch(function(error) {
    print("findResources() returned an error: " + error.name);
});

